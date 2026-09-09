#pragma once
// Engine-neutral value types. NOTHING here may reference Ogre, Qt or GL.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jahshaka { namespace engine {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

struct Colour {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    Colour() = default;
    Colour(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    /// EXACT float equality, on purpose: callers use this to answer "is this the
    /// same value I already pushed?", where a tolerance would let a slider
    /// creep without ever reaching the backend.
    bool operator==(const Colour &o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    bool operator!=(const Colour &o) const { return !(*this == o); }
};

/// Rotation as a unit quaternion. Identity by default.
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    Quat() = default;
    Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

/// Opaque handles to a Scene's meshes and materials. 0 is "none". Per-Scene and
/// monotonic like NodeId. A mesh or material may be shared by any number of nodes.
using MeshId     = unsigned int;
using MaterialId = unsigned int;

/// CPU-side triangle mesh, the shape the assimp importer produces at import time.
/// positions: xyz per vertex (required). normals: xyz per vertex (optional — smooth
/// normals are generated when empty). uvs: uv per vertex (optional). indices: three
/// per triangle (required).
struct MeshData {
    std::vector<float>    positions;
    std::vector<float>    normals;
    std::vector<float>    uvs;
    std::vector<float>    tangents;   // optional, xyzw per vertex (w = handedness);
                                      // generated from uvs when empty — needed for normal maps
    std::vector<unsigned> indices;
    /// GPU skinning: FOUR bone indices and FOUR weights per vertex, in the same
    /// order as `positions`. Indices name a bone of the SkeletonDesc later passed
    /// to attachSkinnedMesh; weights should sum to 1 (the vertex shader does a
    /// plain weighted sum with no renormalisation — weights that miss shrink or
    /// inflate the character). Both empty for a static mesh; a mesh must carry
    /// them at CREATION time (the vertex declaration is fixed then) or
    /// attachSkinnedMesh refuses it.
    std::vector<unsigned char> blendIndices;
    std::vector<float>         blendWeights;
    /// True for meshes whose vertices will be rewritten after creation via
    /// Scene::updateMeshVertices — the CPU-skinning path. The engine allocates an
    /// updatable vertex buffer instead of an immutable one. Static meshes leave
    /// this false and keep the immutable fast path. GPU-skinned meshes are
    /// IMMUTABLE: the pose reaches the GPU as bone matrices, never as vertices.
    bool dynamic = false;
    size_t vertexCount() const { return positions.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }
    bool hasSkinData() const {
        return !blendIndices.empty() && blendIndices.size() == vertexCount() * 4 &&
               blendWeights.size() == vertexCount() * 4;
    }
};

// ---- Rigs (GPU_SKINNING_SPEC) ----------------------------------------------
/// One bone of a rig, in its BIND pose. The transform is LOCAL to the parent
/// bone; a root bone (parent < 0) is local to the mesh node the rig deforms.
struct BoneDesc {
    std::string name;
    int         parent = -1;      ///< index into SkeletonDesc::bones, -1 = root
    Vec3        bindPosition;
    Quat        bindRotation;
    Vec3        bindScale{1.0f, 1.0f, 1.0f};
};

/// A rig: bones in the order MeshData::blendIndices names. Order is otherwise
/// free — a parent may follow its child — but the hierarchy must be acyclic.
///
/// `id` must be derived from the STRUCTURE ONLY — the ordered bone names, the
/// hierarchy and the bind transforms — and never from the source file, the
/// clip set or anything else. The backend caches the translated rig by this id
/// for the life of the process, so (a) two loads of the same rig from different
/// files must resolve to ONE cached rig (which is what lets clips authored in
/// one file drive a character loaded from another), and (b) a rig that differs
/// in any bone must get a different id or it silently aliases the cached one.
struct SkeletonDesc {
    std::string           id;
    std::vector<BoneDesc> bones;
};

/// What a scene's rigs COST right now (AVATAR_RIG_PERF_SPEC §3.5).
///
/// Every field is a number the rig-perf program makes a claim about, so every
/// field is readable from a suite rather than inferred from a log:
///
///   rigged        nodes carrying a skinned renderable.
///   instances     DISTINCT SkeletonInstances behind them — the count Ogre
///                 evaluates in updateAllAnimations, i.e. one per PIECE until
///                 pieces share, one per CHARACTER after.
///   shared        nodes rendering from somebody else's instance
///                 (`rigged - instances` unless a follower's master went away).
///   streamedBones the bone matrices HlmsPbs streams per pass, summed over the
///                 rigged nodes: each node contributes the length of its
///                 blend-index map, which is the whole rig under the identity
///                 map and only the piece's own bones under a compacted one.
struct RigStats {
    size_t rigged = 0;
    size_t instances = 0;
    size_t shared = 0;
    size_t streamedBones = 0;
};

/// A posed bone: LOCAL to its parent bone (a root bone: local to the mesh node).
/// This is absolute local TRS, not a delta from the bind pose.
struct BonePose {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// ---- Clips (ANIMATION_ENGINE_MIGRATION_SPEC) --------------------------------
/// One key of one bone track. The TRS is ABSOLUTE and LOCAL TO THE PARENT BONE
/// — the host has already composed away any pivot chain the source file had
/// (iris::ClipExtractor). Times in SECONDS, strictly increasing.
///
/// NOT a delta from the bind pose: the backend converts to the bind-relative
/// form the engine accumulates in, because that conversion needs the rig's bind
/// pose and the boundary should not make the host carry it twice.
struct BoneKey {
    float time = 0.0f;
    Vec3  position;
    Quat  rotation;
    Vec3  scale{1.0f, 1.0f, 1.0f};
};

/// One bone's track. `bone` indexes SkeletonDesc::bones.
struct BoneTrack {
    int                  bone = -1;
    std::vector<BoneKey> keys;
};

/// A clip, ready to attach to a node's rig.
///
/// `id` MUST be a content hash of the rig id and every track. The backend
/// caches the translated clip under it for the LIFE OF THE PROCESS and the
/// cache is by name, so an id derived from anything else (a file path, a clip
/// name, an asset guid) makes a re-imported clip alias the old one forever —
/// the same failure mode as the VCT datablock-pointer cache.
///
/// `name` is what setClipStates and clipNames speak. It is uniquified per node
/// at attach time if it collides, and the mapping is reported by clipNames.
///
/// `length` in seconds. A length <= 0 is PADDED to a minimum, not refused:
/// every Mixamo character download ships a single-frame T-pose clip and it is
/// the one the UI selects by default. Engine-side a zero length is fmod(t, 0)
/// = NaN, so the padding is not cosmetic.
struct ClipDesc {
    std::string            id;
    std::string            name;
    float                  length = 0.0f;
    std::vector<BoneTrack> tracks;
};

/// What the host asserts about one clip, this frame. ABSOLUTE time only —
/// there is deliberately no addTime on this boundary, because a relative clock
/// makes every pose assertion order-dependent.
///
/// `weight` is raw INTENT. The backend normalizes PER BONE from the clips'
/// coverage (a bone only clip A animates gets all of A at any weight split,
/// never half of it) and reports the result through clipBoneWeights.
struct ClipState {
    std::string name;
    bool        enabled = true;
    float       time    = 0.0f;
    float       weight  = 1.0f;
    bool        looping = true;
};

using TextureId = unsigned int;
enum class SkyMode { NoSky, Equirectangular, Cubemap };   // 'None' collides with X11's macro

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
///   * `twoSided` and `alphaCutoff`, which behave as they always do.
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
    bool   twoSided    = false;  ///< draw and light both faces (no back-face culling)
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
               alphaCutoff == o.alphaCutoff && twoSided == o.twoSided &&
               normalMapWeight == o.normalMapWeight &&
               // ELEMENT-WISE, deliberately: `uvScale == o.uvScale` on arrays
               // compares the two ADDRESSES, which are never equal, and the
               // guard would silently re-push every material every frame.
               uvScale[0] == o.uvScale[0] && uvScale[1] == o.uvScale[1] &&
               uvOffset[0] == o.uvOffset[0] && uvOffset[1] == o.uvOffset[1] &&
               uvRotation == o.uvRotation &&
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

/// One camera-facing textured quad in a node's billboard set (Scene::setBillboards).
/// Positions are WORLD-space: the document simulates particles in world space and
/// the engine draws them as-is.
struct BillboardInstance {
    Vec3   position;                       ///< world-space centre of the quad
    float  size = 1.0f;                    ///< quad edge length in world units
    float  rotationRadians = 0.0f;         ///< spin around the view axis
    Colour colour = Colour(1.0f, 1.0f, 1.0f, 1.0f);   ///< multiplies the texture
};

/// WHICH LAYER a billboard set belongs to — scene content, or editor helper.
///
/// `Scene` is what a particle emitter is: geometry in the world, depth-tested
/// against it, graded by every post effect the view runs (tonemap, bloom, SSAO,
/// SMAA) exactly like the meshes around it.
///
/// `Overlay` is what a light ICON is: a helper the user must be able to READ.
/// It draws in the same pass as the gizmo — after the whole post chain, with no
/// depth test — so a white glyph stays white instead of being tonemapped to
/// grey, bloomed into its neighbours and smeared by edge detection (the
/// 2026-09-08 owner report: "the light icons are grey and blurred"). Pair it
/// with Scene::setNodeHelper so the icon also stays out of reflections and
/// probe captures.
enum class BillboardLayer { Scene, Overlay };

// ---- Particles (PARTICLES_FX2_SPEC.md): natively simulated particle systems ----
// The host describes WHAT it wants; the engine owns every Ogre object behind it.
// One authored node = one particle-system definition = one quota, one material,
// one visibility flag (the definition is what the render queue tests).

/// The shape particles spawn inside. Point ignores `extents`.
enum class ParticleEmitterShape { Point, Box, Cylinder, Ellipsoid, HollowEllipsoid, Ring };

/// How a particle's quad is oriented. `Point` is the camera-facing billboard
/// everything used before this; `OrientedSelf` streaks the quad along the
/// particle's own velocity (sparks, rain).
enum class ParticleOrientation { Point, OrientedCommon, OrientedSelf,
                                 PerpendicularCommon, PerpendicularSelf };

/// One emitter on a system. Position/direction are LOCAL to the node: the engine
/// applies the node's derived position and orientation (but NOT its scale — an
/// emitter's spawn volume is numeric, see `extents`).
struct ParticleEmitterDesc {
    ParticleEmitterShape shape = ParticleEmitterShape::Point;
    Vec3  position{0, 0, 0};       ///< offset from the node's origin
    Vec3  direction{0, 1, 0};      ///< emission axis; the document's +Y convention
    float angleDegrees = 0.0f;     ///< emission cone half-angle around `direction`
    float rate = 24.0f;            ///< particles per second
    float velocityMin = 1.0f, velocityMax = 1.0f;   ///< initial speed range (m/s)
    float ttlMin = 1.0f, ttlMax = 1.0f;             ///< time-to-live range (seconds)
    float sizeWidth = 1.0f, sizeHeight = 1.0f;      ///< initial quad dimensions
    Colour colourStart{1, 1, 1, 1}, colourEnd{1, 1, 1, 1};   ///< per-particle emission colour range
    Vec3  extents{1, 1, 1};        ///< Box: w/h/d. Cylinder/Ellipsoid/Ring: radii. Point: ignored
    Vec3  innerExtents{0, 0, 0};   ///< HollowEllipsoid / Ring only: the hole
    float duration = 0.0f;         ///< 0 = emit forever; >0 = burst of this many seconds
    float repeatDelay = 0.0f;      ///< pause between bursts
    float startTime = 0.0f;        ///< delay before the first emission
};

/// One affector on a system. `kind` selects which fields matter; the rest are
/// ignored. Affectors run per particle, per frame, SIMD, on worker threads.
struct ParticleAffectorDesc {
    enum class Kind {
        ColourKeys,     ///< colour over life, up to 6 keys (ColourInterpolator)
        ScaleKeys,      ///< size multiplier over life, up to 6 keys (ScaleInterpolator)
        Rotator,        ///< random start angle + spin speed
        LinearForce,    ///< a constant acceleration: gravity, buoyancy, wind
        Turbulence,     ///< random velocity perturbation (DirectionRandomiser)
        DeflectorPlane, ///< bounce off an infinite plane
        // ---- ADDENDUM A-4: the four the plugin registers and we never used.
        // (The plugin has TEN affector factories; we mapped six, and the audit's
        // "6/9" counted the two colour faders as one.)
        /// Per-second colour DELTAS, in two stages: `colourAdjust1` until a
        /// particle has `colourSwitchAt` seconds of life left, `colourAdjust2`
        /// after. Covers the plugin's plain ColourFader as well — it IS this
        /// affector with adjust2 == adjust1 — so there is one kind, not two.
        /// Different from ColourKeys: keys REPLACE the colour at authored life
        /// fractions, a fade ADDS a rate to whatever the colour currently is.
        ColourFade,
        /// Colour over life sampled from row 0 of an IMAGE — the classic fire
        /// ramp. NOT a TextureId: the affector loads by NAME through
        /// ResourceGroupManager::AUTODETECT, so the engine registers the file's
        /// directory as a resource location first (the idiom OgreLights and
        /// OgreDecals already use for the same reason).
        ColourRamp,
        /// A size RATE: additive units per second, or multiplicative
        /// `rate^dt` when `scaleMultiply` is set. Different from ScaleKeys,
        /// which authors absolute sizes at life fractions.
        ScaleRate
    };
    Kind kind = Kind::LinearForce;

    /// ColourKeys / ScaleKeys. `keyCount` entries are used, in ascending time.
    /// Times are life fractions in [0,1]. Colour components may exceed 1 — the
    /// GPU encoding carries [-4, 120], which is what makes HDR fire bloom.
    unsigned keyCount = 0;
    Colour colourKeys[6];
    float  colourKeyTimes[6] = {0, 0, 0, 0, 0, 0};
    float  scaleKeys[6]      = {1, 1, 1, 1, 1, 1};
    float  scaleKeyTimes[6]  = {0, 0, 0, 0, 0, 0};

    /// Rotator: degrees. Start angle is picked per particle in [rotStart, rotEnd],
    /// spin speed per particle in [rotSpeedMin, rotSpeedMax] degrees/second.
    float rotSpeedMin = 0.0f, rotSpeedMax = 0.0f;
    float rotStart = 0.0f, rotEnd = 0.0f;

    /// LinearForce: world-space acceleration. `forceAverage` averages the force
    /// into the velocity instead of adding to it.
    Vec3 force{0, 0, 0};
    bool forceAverage = false;

    /// Turbulence: how much random direction is injected, and to what fraction
    /// of the particles (`scope` in [0,1]).
    float randomness = 0.0f, scope = 1.0f;
    bool  keepVelocity = false;

    /// DeflectorPlane.
    Vec3  planePoint{0, 0, 0}, planeNormal{0, 1, 0};
    float bounce = 1.0f;

    // ---- ADDENDUM A-4 ----------------------------------------------------
    /// ColourFade: per-second deltas, stage 1 then stage 2, plus the clamps.
    /// Both stages default to ZERO, which is the neutral value — an affector
    /// with no authored fade does nothing at all.
    Colour colourAdjust1{0, 0, 0, 0};
    Colour colourAdjust2{0, 0, 0, 0};
    /// Switch to stage 2 when the particle has this much life LEFT, in
    /// seconds. 0 = never switch (stage 1 for the whole life).
    float  colourSwitchAt = 0.0f;
    Colour colourMin{0, 0, 0, 0};
    Colour colourMax{1, 1, 1, 1};

    /// ColourRamp: an absolute path to a 1-D ramp image. Row 0 is sampled
    /// across the particle's life.
    std::string colourRampPath;

    /// ScaleRate: units per second (additive) or the per-second factor
    /// (multiplicative). 1.0 with `scaleMultiply` and 0.0 without are both
    /// neutral.
    float scaleRate = 0.0f;
    bool  scaleMultiply = false;
};

/// A complete particle system for one node. Changing a scalar (rate, colour keys,
/// force...) is applied in place; changing the TOPOLOGY — the emitter shapes, the
/// affector kinds, the quota, the orientation — rebuilds the underlying definition,
/// which is why the engine keeps the affector set fixed and neutral at defaults.
struct ParticleSystemDesc {
    unsigned  quota = 1024;        ///< hard cap on live particles; rounded up to a bucket
    TextureId texture = 0;         ///< 0 = untextured white
    bool      additive = true;     ///< (src-alpha, one); false = alpha blending
    bool      alphaHash = true;    ///< order-independent transparency for alpha blending
                                   ///< (ignored when `additive`, which needs no sorting)
    ParticleOrientation orientation = ParticleOrientation::Point;
    Vec3      commonDirection{0, 0, 1}, commonUp{0, 1, 0};   ///< *Common orientations only
    std::vector<ParticleEmitterDesc>  emitters;
    std::vector<ParticleAffectorDesc> affectors;
};

enum class LightType { Directional, Point, Spot, Area };

/// Shadow-map filter quality. GLOBAL to the engine, not per light — the backend's
/// material system has exactly one filter for every shadowed light (see
/// Engine::setShadowFilter). Ordered from cheapest/sharpest to softest.
enum class ShadowFilter { Hard, Soft, VerySoft };

/// A light attached to a node. Direction comes from the node's orientation
/// (lights shine down the node's -Z), position from the node's transform.
struct LightDesc {
    LightType type = LightType::Point;
    Colour    colour = Colour(1.0f, 1.0f, 1.0f);
    float     intensity = 1.0f;        // radiometric scale (Jahshaka's "intensity")
    float     range = 10.0f;           // point/spot falloff distance
    /// The spot cone's HALF angle, in degrees — the angle between the light's
    /// axis and the cone's edge, which is what the document has always stored
    /// and what the editor's cone wire is drawn from
    /// (`radius = range * tan(spotCutOff)`, scenemirror.cpp).
    ///
    /// It is NOT what Ogre's `Light::setSpotlightRange` takes: that one wants
    /// the FULL apex angle. Passing the half angle straight through (which this
    /// boundary did until LIGHTING_FIX fix 5) rendered every spot at half the
    /// cone the editor drew — the classic "the light does not fill its wire"
    /// report. The doubling lives in OgreScene::setLight, once.
    float     spotAngleDegrees = 30.0f;
    /// 0..1. The bright core as a fraction pulled off the outer cone:
    /// innerFull = outerFull * (1 - softness). 0 = hard edge, 0.99 = almost all
    /// penumbra. Values outside the range are clamped, not honoured.
    float     spotSoftness = 0.1f;
    /// The penumbra's exponent, Ogre's `falloff` argument. 1 = linear across the
    /// penumbra (the only value this engine used before it was authorable);
    /// higher concentrates the light towards the core.
    float     spotFalloff = 1.0f;
    bool      castShadows = true;          // ignored for Area (backend cannot shadow them)
    /// STATIC SHADOW MAP (SPECS/SHADOW_TOOLING_SPEC.md §4.3): render this
    /// light's shadow map ONCE and keep it until something invalidates it,
    /// instead of re-rendering it every frame. For a point light that is six
    /// cube-face passes plus a copy saved per frame — the single biggest
    /// shadow saving available for a lamp that does not move.
    ///
    /// The engine invalidates it on its own when the light moves or any of its
    /// parameters change; the HOST must call Scene::dirtyStaticShadows()
    /// whenever the geometry the light sees moves, is attached or is removed
    /// (SceneMirror does). Engine::refreshShadows() re-renders every static map
    /// once, for the "I do not know what changed" case.
    ///
    /// IGNORED for directional lights (PSSM follows the camera) and for area
    /// lights (which never cast). Ignored, not refused: a host that stores the
    /// flag per light must not lose it when a light's type changes.
    bool      shadowStatic = false;
    // Area lights only: a rectangle spanning the node's local X (width) and
    // Z (height), emitting down -Y like every other light type here.
    float     rectWidth = 1.0f;
    float     rectHeight = 1.0f;
    bool      doubleSided = false;         // emit from both faces
    bool      accurate = false;            // physically accurate (LTC) instead of fast approx

    /// Absolute path to an IES photometric profile (.ies); empty = none.
    ///
    /// The profile is a 1-D candela lobe around the light's own direction; the
    /// backend samples it as an extra attenuation term. Three hard limits come
    /// from the renderer, not from us, and the UI must say so:
    ///   * SPOT lights always honour it (shadow-casting or not).
    ///   * POINT lights honour it ONLY while they cast no shadows — a
    ///     shadow-casting point light moves from the clustered light list into
    ///     the pass buffer, whose point-light loop has no profile term.
    ///   * DIRECTIONAL and AREA lights never honour it.
    /// The profile's own candela scale is NOT normalized here: `intensity`
    /// arrives already divided by the profile's peak (the host does that from
    /// import-time metadata) so assigning a profile changes the SHAPE of the
    /// falloff and not the brightness.
    std::string iesProfilePath;

    /// Absolute path to an area-light mask/gobo image; empty = none.
    ///
    /// Honoured ONLY by the fast approximation (`accurate == false`): the LTC
    /// path has no mask term and silently ignores the texture. Every mask in
    /// the process shares ONE fixed-size pooled texture array — the backend
    /// rescales whatever image it is given to the pool's resolution and
    /// generates the full mip chain the diffuse term needs.
    std::string texturePath;

    /// LIGHTING CHANNELS, light side (Scene::setNodeLightMask is the object
    /// side). This light lights an object when `lightMask & object mask` is
    /// non-zero; both default to all-ones, so out of the box every light lights
    /// every object. All 32 bits are the host's — the engine reserves none.
    ///
    /// It filters DIRECT light only, from every path (directional, shadow
    /// casting, Forward+ clustered, area approx and area LTC). It does NOT
    /// filter shadow CASTING (a masked-off object still renders into this
    /// light's shadow map) and it does NOT filter this light's GI bounce.
    /// Scene::setNodeLightMask carries the full contract.
    unsigned  lightMask = 0xFFFFFFFFu;
};

/// A projected-texture decal attached to a node (DECALS_SPEC.md §5.2).
///
/// The decal is an ORIENTED BOX that overwrites base colour, roughness and
/// metalness on every surface inside it. Two conventions, both fixed by the
/// backend's shader and neither cheap to change:
///
///  - it projects down the node's LOCAL -Y (identical to LightDesc's
///    direction convention), and only affects surfaces whose normal points
///    back at it;
///  - the image's U axis is local X and its V axis is local Z, so `width`
///    is the local-X extent and `height` the local-Z extent. `depth` is the
///    local-Y thickness of the projector box.
///
/// `diffuse` MUST come from Scene::loadDecalTexture(): decal images live in a
/// dedicated fixed-geometry texture pool and a plain loadTexture() id is either
/// non-batched (the backend asserts) or in the wrong pool (it would silently
/// sample another decal's image).
///
/// THERE IS NO PER-DECAL OPACITY OR COLOUR TINT. The backend packs exactly four
/// floats per decal (3 rows of the inverse world matrix + one float4 of
/// indices/metalness/roughness); adding either would mean forking the shader
/// template, which this project does not do.
struct DecalDesc {
    TextureId diffuse  = 0;   ///< base colour + alpha mask; from loadDecalTexture()
    TextureId normal   = 0;   ///< optional; from loadDecalTexture(kind Normal)
    TextureId emissive = 0;   ///< optional; from loadDecalTexture(kind Emissive)
    float width  = 1.0f;      ///< local X extent
    float height = 1.0f;      ///< local Z extent
    float depth  = 0.5f;      ///< local Y extent (projection thickness)
    float metalness = 0.0f;
    float roughness = 1.0f;
    /// Diffuse alpha masks the base colour only, not the normal/emissive maps.
    bool  ignoreAlphaDiffuse = false;
};

/// Which pooled decal atlas a decal image is loaded into. The three atlases
/// have different pixel formats and filters (a normal map is neither sRGB nor
/// the same channel layout), so the caller must say which one it wants.
enum class DecalMap { Diffuse, Normal, Emissive };

/// A View's camera. Position/orientation are absolute (the document composes them).
struct CameraDesc {
    Vec3  position;
    Quat  orientation;                 // camera looks down its local -Z
    float fovDegrees = 45.0f;          // vertical
    float nearClip = 0.1f, farClip = 1000.0f;
    bool  orthographic = false;
    float orthoSize = 10.0f;           // HALF the vertical extent when orthographic
                                       // (the document camera's ortho(-s..+s) convention)

    /// LETTERBOX the camera to `aspect` instead of filling the target
    /// (CAMERAS_SPEC §2/§7.4). Off (the default) is the historical behaviour:
    /// the camera's aspect follows the target, so the image always fills it.
    ///
    /// On, the drawn region is the largest `aspect`-shaped rectangle that fits
    /// in the target (or in the PiP rect), centred, with bars in the remainder —
    /// and the camera's own aspect ratio is FROZEN at `aspect` (Ogre's
    /// setAutoAspectRatio goes off), because that is what stops the image being
    /// stretched into the outer rectangle instead of fitted into the inner one.
    bool  constrainAspect = false;
    /// The authored aspect (width / height), used only when constrainAspect.
    float aspect = 16.0f / 9.0f;

    /// THE WIDE-ASPECT FRAMING HOLD (owner report 2026-09-07; RE-SCOPED
    /// 2026-09-08, see below). `fovDegrees` is VERTICAL, so the HORIZONTAL
    /// angle it produces grows with the target's aspect: the default 45-degree
    /// explorer is 75 degrees wide at 16:9 and 118 degrees wide at 32:9 — a
    /// fisheye nobody asked for, on exactly the monitors people buy to see more
    /// of a scene.
    ///
    /// A positive value is THE ASPECT THIS CAMERA'S FRAMING IS HELD AT. At or
    /// below it NOTHING HAPPENS — `fovDegrees` reaches setFOVy bit-for-bit, so
    /// the picture is exactly the one a camera that never heard of this field
    /// produces. Above it the vertical angle narrows just enough to keep the
    /// HORIZONTAL extent the shot had at that aspect
    /// (verticalFovForFramingAspect below), which is the fisheye fix, scoped to
    /// the ultra-wide and full-screen cases it was always about.
    ///
    /// WHY AN ASPECT AND NOT A DEGREE CAP (the 2026-09-08 owner-blocking
    /// defect). This field first shipped as `maxHorizontalFovDegrees`, a FIXED
    /// 95: any camera whose horizontal angle exceeded 95 had its vertical angle
    /// re-derived, and a 75-degree lens crosses 95 horizontal at 1.42:1 — so
    /// the shipped Grand Showroom rendered at 63 degrees vertical instead of 75
    /// on EVERY ordinary monitor, and every imported asset looked too big. A
    /// degree cap cannot express "leave ordinary windows alone", because what
    /// counts as ordinary depends on the camera's own lens. An aspect can: the
    /// hold point is the same 16:9 for a 30-degree lens and a 90-degree one,
    /// and identity below it is exact rather than approximate.
    ///
    /// Zero — the DEFAULT — is off, which is what every AUTHORED camera gets: a
    /// scene camera's angle is a deliberate lens choice and the engine must not
    /// second-guess it. Only the two FREE cameras (the editor explorer, the
    /// player's fly camera) set it, and only they can, because only their hosts
    /// know they are free.
    float framingAspect = 0.0f;

    /// LENS SHIFT (CAMERA_LENS_SPEC §3), as a FRACTION OF THE FRAME: +0.5 in X
    /// slides the image half a frame to the right without rotating the camera.
    /// Zero (the default) is a centred, symmetric frustum — byte-identical to a
    /// camera that never heard of lens shift.
    ///
    /// THE FRACTION IS WHAT CROSSES THE BOUNDARY, deliberately. The projection
    /// wants a WORLD-SPACE offset at the near plane, and that offset depends on
    /// the field of view, the near distance AND the aspect the view is actually
    /// rendering at — which only the engine knows (an unconstrained view adopts
    /// its target's aspect). A host that pre-multiplied would be shifting by
    /// the wrong amount on every window that is not the shape it assumed.
    float lensShiftX = 0.0f;
    float lensShiftY = 0.0f;

    bool operator==(const CameraDesc &o) const {
        return position.x == o.position.x && position.y == o.position.y &&
               position.z == o.position.z &&
               orientation.x == o.orientation.x && orientation.y == o.orientation.y &&
               orientation.z == o.orientation.z && orientation.w == o.orientation.w &&
               fovDegrees == o.fovDegrees && nearClip == o.nearClip && farClip == o.farClip &&
               orthographic == o.orthographic && orthoSize == o.orthoSize &&
               constrainAspect == o.constrainAspect && aspect == o.aspect &&
               framingAspect == o.framingAspect &&
               lensShiftX == o.lensShiftX && lensShiftY == o.lensShiftY;
    }
    bool operator!=(const CameraDesc &o) const { return !(*this == o); }
};

/// The vertical angle of view that holds this camera's HORIZONTAL extent at
/// the one it has on a frame of `framingAspect` — CameraDesc::framingAspect.
///
///     tan(v'/2) = tan(v/2) * framingAspect / aspect     when aspect > framingAspect
///     v'        = v                                     otherwise, EXACTLY
///
/// The first line is the two standard cross-axis identities composed and
/// simplified — hold h, solve for v:
///
///     h  = 2 * atan( tan(v/2) * framingAspect )    the extent being held
///     v' = 2 * atan( tan(h/2) / aspect )           the angle that holds it
///
/// The hold only ever NARROWS, and only STRICTLY ABOVE the framing aspect: on
/// a 16:9 hold every 16:10, 3:2, 4:3 and 16:9 target takes the early return
/// and the authored angle is passed through with NO ARITHMETIC AT ALL, which
/// is what lets every existing 16:9 pixel assertion stay byte-exact (the
/// aspect comparison is exact; a degree-cap round trip was not). Free (a
/// non-positive framing aspect or aspect) is likewise the identity.
///
/// A free function, in the header, deliberately: this is the whole of the
/// policy, and a suite can drive it across an aspect sweep with no engine at
/// all (tests/cameras' framing-hold case).
inline float verticalFovForFramingAspect(float vfovDeg, float aspect, float framingAspect) {
    if (!(framingAspect > 0.0f) || !(aspect > 0.0f) || !(vfovDeg > 0.0f)) return vfovDeg;
    if (aspect <= framingAspect) return vfovDeg;   // inside the hold: untouched, bit for bit
    const float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float haveHalf = std::min(vfovDeg, 179.0f) * 0.5f * kDegToRad;
    const float wantHalf = std::atan(std::tan(haveHalf) * (framingAspect / aspect));
    return std::max(1.0f, wantHalf * 2.0f / kDegToRad);
}

/// THE PICTURE-IN-PICTURE INSET (CAMERAS_SPEC §7.7): a second camera's view of
/// the SAME scene, composited into a rectangle of this View's target.
///
/// Mechanism, and why it is this one: Ogre's own split-screen recipe — a SECOND
/// CompositorWorkspace on the SAME render target, placed by addWorkspace's
/// vpOffsetScale and moved live by setViewportModifier. Route A of
/// CAMERAS_SPEC §7.2, proven on Vulkan at our pin by
/// spikes/camera-pip-vulkan/FINDINGS.md before a line of this was written.
///
/// What the spike found, in one place, because every one of them is a rule this
/// struct's implementation obeys:
///   * the inset workspace must be LAST on the target, and there is no reorder
///     API — every rebuild of the main workspace re-appends it, so the inset is
///     removed and re-added after each one (OgreView::attachWorkspace);
///   * everything it writes ON THE WINDOW must LOAD colour (a Clear on Vulkan
///     is full-target and would wipe the main frame), and the inset needs its
///     OWN BACKGROUND for the same reason: a loaded attachment shows the main
///     image wherever the inset draws nothing;
///   * addWorkspace's vpModifierMask defaults to 0x00, which silently makes the
///     rect inert — it is passed 0xFF;
///   * the main chain's final pass must keep MSAA samples AND resolve them
///     (chain::kMultiWorkspaceStore) or the inset destroys the frame at 4x;
///   * cameras are POOLED and destroyed AFTER the workspace that names them
///     (the other order segfaults on the next frame).
///
/// ROUTE C, since the inset learned to grade (`tonemap`, CAMERAS_SPEC §7.2):
/// the scene pass no longer draws into the window at all. It renders into a
/// LOCAL texture sized from the inset's rectangle — its own depth, its own
/// clear, its own colour space — and a QUAD composites that texture into the
/// window at the rect, either through `HDR/FinalToneMapping` (graded) or
/// through `Ogre/Copy/4xFP32` (not). Three consequences worth stating:
///   * the local texture is sized by FRACTION of the target, so it follows a
///     window resize for free and a RECT MOVE costs nothing; only a rect
///     RESIZE re-creates it (OgreView::applyPip, checked in whole pixels so a
///     steady inset never rebuilds anything — View::pipGeneration proves it);
///   * the camera's aspect is set EXPLICITLY from the rectangle.
///     setAutoAspectRatio would take the LOCAL TEXTURE's aspect instead, which
///     is the same number only by construction and stops being one the moment
///     the texture is rounded up to whole pixels;
///   * the inset's own background is graded with it — the swatch goes through
///     the same tonemapping quad, so the letterbox bars around a constrained
///     shot cannot disagree with the background inside it.
///
/// COST: a second cull and a second render of everything the inset camera sees.
/// The spike measured +0.33 ms/frame CPU on a trivial scene, plus (Route C) one
/// texture the size of the inset and two quads over its rectangle. It is off
/// unless a host asks for it, and the editor asks only while a camera is
/// selected.
///
/// IGNORED ON OFFSCREEN VIEWS unless `allowOffscreen` — the same guarantee, in
/// the same single place, as PostFxDesc and ViewOverlayDesc. Thumbnails,
/// previews and every pixel suite must stay byte-identical whatever a host
/// pushes (tests/engine's pip_is_ignored_offscreen_unless_asked).
struct ViewPipDesc {
    /// Draw the inset at all. False is free — the workspace is not even built.
    bool enabled = false;
    /// The camera the inset renders from. Absolute, exactly like View::setCamera.
    CameraDesc camera;
    /// The inset rectangle in NORMALISED target coordinates: (0,0) is top-left,
    /// (1,1) bottom-right. Defaults to a bottom-right inset a bit under a third
    /// of the view (CAMERAS_SPEC D3).
    float left = 0.66f, top = 0.64f, width = 0.31f, height = 0.33f;
    /// The inset's own background, painted at the rect before the scene renders
    /// — and the letterbox bars when the camera constrains its aspect. Without
    /// it the main image shows through wherever the inset scene does not draw.
    Colour background { 0.06f, 0.06f, 0.07f, 1.0f };

    /// THE offscreen opt-in — same meaning and same word as
    /// PostFxDesc::allowOffscreen and ViewOverlayDesc::allowOffscreen.
    bool allowOffscreen = false;

    /// GRADE THE INSET (CAMERAS_SPEC §7.2 Route C, POST_CHAIN_SPEC §14).
    ///
    /// The inset does not render into the window any more: it renders into a
    /// LOCAL texture (RGBA16F while this is on) and is composited through the
    /// SAME `HDR/FinalToneMapping` quad the main chain uses. Without it a view
    /// whose chain tonemaps showed a RAW linear inset beside a graded main
    /// image — everything above 1.0 clipped to flat white while the viewport
    /// rolled the same highlight off. That is item 6's fourth consumer, and
    /// this is the same secondary-surface answer the thumbnails got.
    ///
    /// A host sets it from the description the view's own chain resolved to
    /// (`PostFxDesc::hdr`), so the inset grades exactly when the main view
    /// does. It is a SHAPE flag: flipping it rebuilds the inset's workspace.
    bool tonemap = false;
    /// The inset's exposure, on the chain's natural-log axis (the same units
    /// and the same meaning as PostFxDesc::exposure, NOT stops).
    ///
    /// ALWAYS FIXED, never measured: the auto path's luminance reduction rides
    /// PROCESS-GLOBAL material parameters and a wall-clock adaptation, which on
    /// a second surface is neither assertable nor per-camera. So the inset uses
    /// the fixed-exposure form of the same tonemapper (POST_CHAIN_SPEC §14), and
    /// this number IS its grade — which is what makes a PIPPED camera's own
    /// exposure visible in the inset without touching the main view. Live: a
    /// change rewrites one clear colour, never a rebuild.
    float exposure = 0.0f;

    bool operator==(const ViewPipDesc &o) const {
        return enabled == o.enabled && camera == o.camera && left == o.left && top == o.top &&
               width == o.width && height == o.height && background == o.background &&
               allowOffscreen == o.allowOffscreen && tonemap == o.tonemap &&
               exposure == o.exposure;
    }
    bool operator!=(const ViewPipDesc &o) const { return !(*this == o); }
};

/// Native window handle a View renders into (X11 Window / HWND / NSView).
///
/// macOS: pass the host's `NSView*` (Qt: `QWidget::winId()`, a QNSView). The
/// backend adds its OWN CAMetalLayer-backed child view under it and presents
/// there — it never takes over the host's layer, which Qt refuses anyway. An
/// `NSWindow*` (its contentView is used) or a ready-made `CAMetalLayer*` (the
/// host then owns its size and contentsScale) are accepted too.
using NativeWindowHandle = unsigned long long;

/// Native display connection (X11 Display*). MUST be the host's own connection —
/// opening a second connection to the same windows causes flicker and cross-bleed
/// between windows. 0 where the platform has no such concept.
///
/// KNOWN LEAK (audit): this is an X11 concept in a supposedly platform-neutral
/// boundary. Left as-is for now; it is only consumed by on-screen Views on
/// Linux/Vulkan and is redesigned when the macOS/Windows hosts arrive.
using NativeDisplayHandle = unsigned long long;

/// Opaque handle to something in a Scene. 0 is "none". Ids are per-Scene and
/// monotonic: a removed node's id is NEVER reused, so a stale id is harmless.
using NodeId = unsigned int;

// ---- Global illumination (scene-level, GI_SPEC.md) ----
/// Which GI system lights the scene. Off is the default everywhere — GI must
/// never cost anything unless the author turns it on.
enum class GiMode {
    Off,
    InstantRadiosity,   ///< bounced light as virtual point lights (VPLs)
    Vct,                ///< voxel cone tracing over the GI bounds (diffuse + specular GI)
    VctPccHybrid        ///< VCT plus parallax-corrected cubemap probes: probe reflections
                        ///< near geometry, cone-traced reflections far from it
};
/// Coarse quality dial; each backend maps it to its own knobs (VPL/ray budget,
/// voxel resolution, probe grid).
enum class GiQuality { Low, Medium, High };

/// A three-state knob whose default answer is "whatever the quality dial says".
/// Used by the hybrid's two expensive probe-capture options
/// (REFLECTIONS_ADOPTION_SPEC.md P3a/P3b): both derive from GiQuality::High by
/// default, and both can be pinned either way independently of it — which is
/// what lets a suite measure ONE of them at a time instead of measuring "High".
enum class GiToggle { Auto, Off, On };

/// Scene-level GI state, pushed idempotently via Scene::setGlobalIllumination.
struct GiParams {
    GiMode    mode    = GiMode::Off;
    GiQuality quality = GiQuality::Medium;
    /// World-space bounds GI operates in (VCT voxel volume; IR area of interest
    /// for directional lights). min == max means "auto": the backend derives it
    /// from the scene's lit geometry plus a margin.
    Vec3      boundsMin, boundsMax;
    /// Instant Radiosity: the node whose light drives the bounce. 0 means "auto"
    /// (the backend picks the first directional light, else any light).
    NodeId    irLight = 0;
    /// Total light bounces, 1..4 (1 = a single indirect bounce).
    int       numBounces = 1;
    /// Hybrid only: reflection-probe counts along each world axis of the GI
    /// bounds (the parallax-corrected cubemap grid). Clamped to 1..8 per axis.
    int       pccProbesX = 3, pccProbesY = 2, pccProbesZ = 3;

    // ---- Probe capture options (hybrid only; REFLECTIONS_ADOPTION_SPEC P3) ----
    /// HDR probe captures — PFG_RGBA16_FLOAT instead of PFG_RGBA8_UNORM_SRGB.
    /// The main chain renders RGBA16_FLOAT, so an LDR probe clips every
    /// highlight BEFORE the IBL convolution that blurs it across the mip chain;
    /// in a room the lamps and windows ARE the reflection content. Costs 2x the
    /// probe VRAM (High/512: ~151 -> ~302 MB at 18 probes), which is why Auto
    /// means "on at GiQuality::High only".
    GiToggle  probeHdr = GiToggle::Auto;
    /// Shadowed probe captures — the probe's six face renders run the scene's
    /// shadow node with `recalculate`, so the reflection contains the room's
    /// shadows instead of a uniformly lit room. Multiplies each face render by
    /// the shadow pass count; Auto means "on at GiQuality::High only". Falls
    /// back silently to the unshadowed capture when no shadow node exists (a
    /// headless engine has none) — GiStatus reports what actually happened.
    GiToggle  probeShadows = GiToggle::Auto;
    /// How far each probe's influence volume is stretched past its 1/N share of
    /// the probe region, so neighbours blend instead of showing a hard seam.
    /// 1.0 = no overlap (visible seams), the pin's own ctor default is 1.5, and
    /// upstream's sample ships 1.25 — which is ours (P3c): less probe-count
    /// pressure on the Forward+ cubemap slots for blending that is already
    /// smooth. Valid range (0, inf).
    float     probeOverlap = 1.25f;
    /// Probe shrink-fit snapping, applied uniformly to all three axes. After
    /// the depth readback re-fits each probe to the geometry around it, a face
    /// that landed within this RELATIVE error of the probe region snaps back
    /// out to it — the cure for "the wall itself has no reflection" when the
    /// fit stops a hair short of it. `snapSidesMin/Max` are the same idea for
    /// probes sitting on the region's own faces/corners, where a much larger
    /// error is safe (upstream reasons about it at length in
    /// PccPerPixelGridPlacement::setSnapSides). These were never set before —
    /// they are the pin's ctor defaults, now OURS and explicit, so an upstream
    /// bump cannot move probe placement without a diff saying so.
    float     probeSnapDeviation = 0.05f;
    float     probeSnapSidesMin  = 0.25f;
    float     probeSnapSidesMax  = 0.25f;
    /// THE GI UPDATE BUDGET (FIX WAVE B1/B2, 2026-09-07) — how many probe
    /// UPDATES the renderer may spend per frame. It replaces P5a's
    /// `dynamicProbes` ("keep the nearest N live for ever") and the document's
    /// old giAutoRefresh flag, which were two spellings of the same question.
    ///
    /// A RATE, not a subset. Each frame the engine dirties the `updateBudget`
    /// highest-priority probes that still owe the current sweep an update, and
    /// refills the sweep when it empties — so every probe in the grid re-captures
    /// within ceil(probeCount / updateBudget) frames, whatever the priority does.
    /// Priority (staleness x proximity to the tracked camera x covers-something-
    /// that-just-moved) only decides the ORDER inside a sweep, which is what puts
    /// the probes the viewer can see, and the ones the moving object is inside,
    /// at the front of it.
    ///
    /// 0 = PAUSED: no probe re-captures, and the mirror stops auto-refreshing GI
    /// as well (it is the same "GI is frozen" intent). That is the pre-fix-wave
    /// shipped behaviour, kept as one switch.
    ///
    /// 1 (the default) is a realtime editor: one probe face-set per frame,
    /// measured at ~2.1 ms in a Debug build at Medium quality (256px faces).
    /// Raising it buys latency at a linear cost. `GiStatus::probeUpdatesPerFrame`
    /// reports the resolved figure (clamped to the probes that exist).
    ///
    /// LOUD CONSEQUENCE, because it changes the picture: while this is above 0
    /// the PCC/VCT trust window is inverted (P5a's finding — see the long note in
    /// OgreGi.cpp buildPcc). Probes are fresher than the voxel volume by
    /// construction once they re-capture, so a probe that covers a pixel wins it,
    /// and ROUGH surfaces inside the probe region take their environment from the
    /// probes instead of from cone tracing. Mirror-sharp pixels do not move.
    int       updateBudget = 1;
    /// VCT light-injection ray-march step scale AT REST (FIX WAVE B5). Upstream:
    /// "bigger values means the shadow raymarching during light injection is
    /// faster, but may cause glitches if too high (areas that are supposed to be
    /// shadowed won't be shadowed)"; below 1.0 trips an assert, so 1.0 is the
    /// floor as well as the default. The engine RAISES it on the cheap in-motion
    /// re-injection path only (see OgreScene::giRayMarchStepScale) — a re-inject
    /// that happens every few frames of a drag is allowed to be coarse; the
    /// re-solve that lands when the drag stops is not.
    float     rayMarchStepScale = 1.0f;
    /// DDGI — the irradiance-field diffuse layer (GI_UNIFIED_SPEC.md §4 P1).
    ///
    /// On, and in a VCT mode, the engine builds an `Ogre::IrradianceField` over
    /// the SAME voxel volume VCT already lit (Majercik et al.: octahedral
    /// irradiance + depth-visibility probes, cone-traced out of VctLighting) and
    /// binds it to HlmsPbs. It is a DIFFUSE layer only: VCT keeps the specular
    /// cones, the probes/planar/SSR keep everything they had.
    ///
    /// THE ONE THING TO KNOW BEFORE TURNING IT ON: binding a field makes
    /// HlmsPbs set `VctDisableDiffuse`, so DDGI REPLACES the voxel-cone diffuse
    /// rather than adding to it. The replacement is smooth and leak-resistant
    /// where the cone-traced term blew out corners, and — once the pass-buffer
    /// alignment defect this lane found is corrected (FogHlmsListener::
    /// ifdAlignFloats) — it lands within about 15% of the brightness it takes
    /// over from, which is what makes `ddgiIntensity` a trim rather than a
    /// correction.
    ///
    /// GiToggle::Auto means "let the quality tier decide", and the deciding
    /// happens DOCUMENT-SIDE: the Rayon tier (GI_UNIFIED P2) writes a concrete
    /// on/off through into the document field the mirror pushes here, so Auto
    /// reaching the engine means "no tier was ever applied to this scene" and
    /// resolves to OFF — which is what makes every already-serialized scene
    /// render exactly as it did before this feature existed.
    ///
    /// Ignored outside GiMode::Vct / GiMode::VctPccHybrid: the field is fed by
    /// VctLighting, so there is nothing to build without a voxel volume. A
    /// DDGI-only mode is deliberately NOT offered — with no VCT bound the
    /// shader's ambient gate (`@property(vct_num_probes) if(vctSpecular.w==0)`)
    /// disappears and the sky/flat ambient would be counted twice on top of the
    /// field's own diffuse (P0 spike §5, measured).
    GiToggle  ddgi = GiToggle::Auto;
    /// The DDGI diffuse INTENSITY — ours, not upstream's (IrradianceFieldSettings
    /// has no such knob; ours rides the pass buffer into
    /// media/Hlms/Jahshaka/JahIfd_piece_ps.any, so changing it is a const-buffer
    /// write and never a shader rebuild).
    ///
    /// 1.0 is upstream's raw brightness, and it is also the CALIBRATED default:
    /// measured on the gi.modes room, the field's red bounce at 1.0 is 0.145
    /// against the VCT diffuse's 0.169 that it replaces — 86%, the same visual
    /// class, no trim needed. (The P0 spike's "~13x dimmer" reading was an
    /// artifact of the pass-buffer misalignment described on
    /// FogHlmsListener::ifdAlignFloats, which was collapsing every irradiance
    /// lookup onto one texel; it is corrected here and the number does not
    /// survive it. GI_UNIFIED_SPEC addendum item 2 should be read with that in
    /// mind.) The knob stays because the two terms are different integrals and
    /// a scene may want the trim; clamped to [0, 64], and 0 is a legitimate
    /// "field bound, contributing nothing" for A/B measurement.
    float     ddgiIntensity = 1.0f;
    /// THE AMBIENT SKY-VISIBILITY STRENGTH — the Rayon ambient fix's one dial
    /// (GI_UNIFIED_SPEC.md ADDENDUM CORRECTION; the mechanism is documented at
    /// length on media/Hlms/Jahshaka/JahIfd_piece_ps.any).
    ///
    /// WHAT IT RESTORES. Inside a VCT volume the shader's own ambient term is
    /// gated off (`if( vctSpecular.w == 0 )`, a gate upstream commented the
    /// volume test out of, so it never fires). The only live ambient was the
    /// cone-traced diffuse's `ambient * escapeFraction`, and binding a field
    /// deletes that branch — so with DDGI on, ambient light inside the volume
    /// came from nowhere: 15-25% darker mid-ground on OPEN scenes, sealed rooms
    /// unaffected. This scales the replacement: the scene's SH ambient times a
    /// sky-visibility fraction read out of the field's OWN depth atlas (one tap
    /// per cage probe along the surface normal; a probe whose ray left the
    /// volume without hitting anything votes "sky").
    ///
    /// 1.0 is the honest reconstruction and the default. 0 removes the term
    /// entirely — through a UNIFORM shader branch, so it is also exactly "DDGI
    /// as it behaved before this fix", which is what makes the A/B in
    /// gi.ddgi_ambient (and the sealed-room invariance assertion) possible.
    /// Above 1 it is a stylistic sky-fill trim, like ddgiIntensity is for the
    /// bounce; clamped to [0, 8].
    ///
    /// It is DELIBERATELY not folded into ddgiIntensity: that one scales
    /// bounced light and this one scales ambient, they are different integrals,
    /// and folding them would make "turn the fix off" impossible without also
    /// turning the field's own contribution off.
    ///
    /// Ignored when no field is bound (nothing to correct: outside a VCT scene
    /// the shader's ambient is live, and inside one without a field the cone
    /// diffuse still carries it).
    float     ddgiAmbient = 1.0f;
};

/// What GI is ACHIEVING, as opposed to what GiParams requested — the same
/// "the renderer beats the request" contract as View::sampleCount() and
/// Scene::activePlanarReflectors().
///
/// It exists because the hybrid could silently degrade: a missing probe
/// workspace definition made buildPcc log a line and return, leaving the scene
/// rendering as plain VCT with every caller (and every test) still believing it
/// had probe reflections (REFLECTIONS_ADOPTION_SPEC.md §3, finding F12). Read
/// this after setGlobalIllumination to assert the hybrid actually armed.
struct GiStatus {
    /// The mode currently in force (what the scene last accepted).
    GiMode mode = GiMode::Off;
    /// Live parallax-corrected cubemap probes. In VctPccHybrid this is the
    /// clamped grid product (pccProbesX * pccProbesY * pccProbesZ); 0 in every
    /// other mode, and 0 in the hybrid when the probe arm failed to build.
    int    probeCount = 0;
    /// Whether THIS scene's probe grid is the one bound to the process-wide
    /// HlmsPbs — i.e. whether probe reflections are actually being sampled.
    /// False when the hybrid degraded to plain VCT, and false when another
    /// scene took the binding over (the sVctBindingOwner rule).
    bool   pccBound = false;
    /// Whether this scene owns the process-wide VCT lighting binding.
    bool   vctBound = false;
    /// The RESOLVED lit volume — what the voxelizer was actually given, after
    /// the explicit-bounds check, the per-node exclude flag and the extent
    /// outlier rejection (REFLECTIONS_ADOPTION_SPEC.md P1a). Equal corners mean
    /// "no volume" (GI off, or nothing to light). This is the only way to see
    /// what the auto-fit decided: the document's giBounds rows stay at 0,0,0
    /// for every scene that never pinned them.
    Vec3   boundsMin;
    Vec3   boundsMax;
    /// The RESOLVED reflection-probe region — the free space the probe grid was
    /// placed in, which is deliberately NOT the lit volume (it carries no
    /// margin and is pulled in to the room's walls). Equal corners in every mode
    /// but the hybrid.
    Vec3   probeRegionMin;
    Vec3   probeRegionMax;
    /// What the probe captures RESOLVED to (REFLECTIONS_ADOPTION_SPEC P3a/P3b),
    /// as opposed to what GiParams::probeHdr/probeShadows asked for: both are
    /// GiToggle::Auto by default, so the request alone never says what happened,
    /// and `probeShadows` additionally falls back to false when the scene has no
    /// shadow node to recalculate. False in every mode but the hybrid.
    bool   probeHdr = false;
    bool   probeShadows = false;
    /// How many probes the renderer re-captures per frame — the RESOLVED
    /// `GiParams::updateBudget`, clamped to the probes that actually exist, and
    /// 0 whenever the probe arm did not build (FIX WAVE B1/B2). 0 in every mode
    /// but the hybrid. Every probe still refreshes within
    /// ceil(probeCount / this) frames; see GiParams::updateBudget.
    int    probeUpdatesPerFrame = 0;
    /// The UNION of every probe's fitted PARALLAX SHAPE — the boxes the shader
    /// reprojects reflection rays onto (FIX WAVE A2). Equal corners in every
    /// mode but the hybrid, and in the hybrid it must lie inside
    /// probeRegionMin/Max: a parallax box bigger than the free space the grid
    /// was fitted to makes the hybrid's trust test reject the probe and hand the
    /// pixel to cone tracing, which in an interior is black. The union is what
    /// giStatus can carry in constant size; the per-probe boxes go to the log
    /// under JAHSHAKA_GI_DEBUG.
    Vec3   probeShapeMin;
    Vec3   probeShapeMax;
    /// THE PER-PROBE LOCALITY CHECK (2026-09-07 fix wave, defect 2b) — and the
    /// reason the union above is NOT a self-check.
    ///
    /// The union is clamped into the probe region by construction, so "the union
    /// is inside the region" is a tautology that can never fire: it stayed
    /// perfectly healthy while every probe in the Grand Showroom had been given
    /// the SAME whole-room parallax box and the metals flickered black. What
    /// matters is per probe and RELATIVE: how far each probe's fitted box
    /// reaches past its own cell (its 1/N share of the region), as a multiple of
    /// that cell's half-extent, worst axis.
    ///
    ///   `worstProbeShapeCellRatio` — the largest such multiple in the grid.
    ///   1.0 means "exactly its own cell"; the fit is ALLOWED to reach further,
    ///   because a probe legitimately sees the whole room's walls, up to
    ///   kProbeShapeCellAllowance (OgreGi.cpp).
    ///   `probesExceedingCell`     — how many probes are past that allowance.
    ///   Non-zero is a defect, not a tuning matter: the shrink-fit has returned
    ///   a box unrelated to the space the probe is responsible for.
    ///
    /// Both are 0 in every mode but the hybrid, and 0 when no probe was placed.
    float  worstProbeShapeCellRatio = 0.0f;
    int    probesExceedingCell = 0;
    /// How many probes the REGION CLAMP had to correct at the last build — how
    /// many times `PccPerPixelGridPlacement`'s shrink-fit returned a parallax
    /// box that was not inside the space the grid was fitted to. It fits from
    /// ONE 1x1 AVERAGED depth value per cube face, which is only meaningful
    /// when every face sees a wall; put a column, a partition or a parked car
    /// in the room and the average means nothing (measured on the shipped Grand
    /// Showroom: boxes reaching +-23.7 units in a room spanning +-12.25). A
    /// high count is not itself an artifact — the clamp corrects it — but it is
    /// the honest signal that the fit is not doing the work in this scene, and
    /// unlike the union check above it CAN fire.
    int    probesClampedToRegion = 0;
    /// The Forward+ per-cell CUBEMAP PROBE budget in force for this scene.
    ///
    /// `ForwardClustered::collectObjsForSlice` writes probes into a cluster cell
    /// only while the cell's count is below this, and drops the rest SILENTLY
    /// (OgreForwardClustered.cpp:291-298). A dropped probe means the pixels in
    /// that cell find no probe covering them and the hybrid hands them to cone
    /// tracing, which in an interior is black — the "hard-edged black rectangles
    /// crawling over the metals" defect of 2026-09-07. The engine therefore
    /// grows this budget to hold the probe grid it just built; a value BELOW
    /// `probeCount` means cells can still drop probes.
    int    cubemapProbeSlotsPerCell = 0;
    /// Whether the LAST full refresh re-used the existing voxel arm instead of
    /// tearing it down and building a new one (FIX WAVE B4). False after a
    /// from-scratch build, which is what every mode change, quality change and
    /// post-destruction flush still does. Exposed because the difference is a
    /// factor of several in refresh cost and is otherwise invisible.
    bool   reusedLastRefresh = false;

    // ---- DDGI / IrradianceField (GI_UNIFIED_SPEC.md §4 P1) ----------------
    // Same "what it ACHIEVED" contract as pccBound above: `GiParams::ddgi` is
    // the request, and it can be refused for reasons the caller cannot see (no
    // VCT volume to feed the field, no IFD media staged, a construction that
    // threw). These four are what the shader is actually doing.

    /// The process-wide HlmsPbs is sampling THIS scene's irradiance field.
    bool   ifdBound = false;
    /// Probes in the field (the product of the three per-axis counts). 0 when
    /// there is no field.
    int    ifdProbes = 0;
    /// Every probe in the field has been integrated at least once since the
    /// last build or reset. A bound field is ALWAYS converged on the frame it
    /// binds (the build converges it in one dispatch); this reads false only
    /// while a progressive re-converge after `refreshGiLighting` is in flight.
    bool   ifdConverged = false;
    /// Probes the field is re-integrating per frame while a re-converge is in
    /// flight — the resolved figure, derived from `GiParams::updateBudget` and
    /// then clamped to the engine's dispatch rule (see OgreGi.cpp
    /// ifdProbesPerFrame: a dispatch of fewer rays than one thread group is an
    /// UNCAUGHT THROW in a release-built engine, so the clamp is mandatory).
    /// 0 when the budget is 0 (paused: nothing re-converges) or when there is
    /// no field.
    int    ifdProbesPerFrame = 0;
};

// ---- Fog (scene-level) ------------------------------------------------------
/// EXPONENTIAL distance fog, plus an optional height-varying layer of the same
/// colour. Both layers absorb, so their transmittances multiply:
///
///     transmittance = 2^( -distance * density ) * 2^( -heightOpticalDepth )
///     pixel         = lerp( colour, surface, transmittance )
///
/// `density` is therefore "how much is lost per world unit" in exp2 units: a
/// surface 1/density units away keeps half its own colour, and 4.32/density is
/// where only 5% of it survives. (The document maps the legacy linear start/end
/// pair onto it by matching the half-fogged distance — iris::Scene.)
///
/// Only lit (PBR) surfaces are fogged; unlit overlays (gizmos, wires,
/// billboards) and the sky never are, exactly as before.
struct FogDesc {
    bool   enabled = false;
    Colour colour;                ///< linear fog colour
    float  density = 0.024f;      ///< homogeneous density per world unit (exp2)

    /// Height layer: a second exponential medium whose density falls off with
    /// world Y — density(y) = heightDensity * 2^( -(y - heightLevel) * heightFalloff )
    /// — integrated along the view ray. heightDensity = 0 disables it exactly
    /// (the shader branch is skipped, not multiplied by one).
    float  heightDensity = 0.0f;  ///< density at heightLevel, per world unit (exp2)
    float  heightFalloff = 0.1f;  ///< per world unit; larger = thins out faster with altitude
    float  heightLevel   = 0.0f;  ///< world Y where heightDensity applies

    /// Brightness breakthrough: bright pixels (a sun disc, an emissive sign)
    /// resist the fog instead of dissolving into it. `breakMinBrightness` is the
    /// luminance where breaking through starts, `breakFalloff` how fast it takes
    /// hold. breakFalloff = 0 turns it off, leaving pure exponential fog.
    float  breakMinBrightness = 0.25f;
    float  breakFalloff       = 0.1f;
};

// ---- Planar reflections (scene-level, PLANAR_REFLECTIONS_SPEC.md) ----
/// Mirrors and glossy floors. A node marked a *reflector* (Scene::setNodePlanarReflector)
/// contributes a world-space reflection PLANE derived from its own flat geometry;
/// surfaces lying on such a plane, and within 20 degrees of its normal, sample a
/// re-render of the scene from the mirrored camera.
///
/// THE COST IS A WHOLE EXTRA SCENE RENDER PER ACTIVE PLANE, every frame — plus a
/// private shadow atlas render when `shadows` is on. A scene may hold any number of
/// reflectors; only `budget` of them (the ones on screen, nearest first) render.
/// budget == 0 disables the feature completely and costs nothing at all.
struct PlanarReflectionParams {
    /// Active reflection planes, 0..8 (0 = off). CHANGING THIS RECOMPILES SHADERS:
    /// the count is baked into the PBS shader as a property, not passed as a
    /// uniform. Pushing the same value again is free.
    int      budget = 0;
    /// Edge of each plane's square render target, 256..2048 (rounded to a power
    /// of two). Memory is budget x resolution^2 x 4 bytes x 4/3 (the mip chain),
    /// allocated whether or not the planes are visible.
    unsigned resolution = 512;
    /// Mip chain on the reflection targets. Mips ARE how glossiness works — the
    /// shader samples at roughness * numMips. Without them a rough floor
    /// reflects as sharply as a mirror. Free to leave on.
    bool     mipmaps = true;
    /// Shadows inside the reflections. Costs a private shadow atlas per plane,
    /// at HALF the scene's shadow resolution, allocated up front.
    bool     shadows = false;
    /// Full lighting update for each reflection camera. Off is faster and rarely
    /// visibly different (Ogre's own words); on is what "maximum realness" means.
    bool     accurateLighting = true;
    /// World-space distance over which a surface's reflection fades out as it
    /// leaves the plane, and the radius within which a surface may be matched to
    /// a plane at all. Small values keep a floor's reflection on the floor.
    float    maxDistance = 2.0f;
    /// Clear colour of the reflection render (what shows where the scene has no
    /// geometry and no sky). Normally the view's background.
    Colour   background = Colour(0.0f, 0.0f, 0.0f, 1.0f);
};

enum class Backend { Vulkan, OpenGL };

/// `Engine::createScene(name, kSceneMainThreadOnly)` — "this scene gets NO
/// worker threads" (SPECS/THREADING_ADOPTION_SPEC.md P5).
///
/// It needs a spelling of its own because the natural one is taken: `0` has
/// always meant "I do not care, give me the backend's default", and every
/// caller that passes nothing passes 0. So the boundary needs two ways to say
/// "zero", and only one of them may reach the backend as zero.
///
/// WHY IT IS NOT JUST A SMALLER NUMBER. At 1 the backend still SPAWNS a worker
/// thread and still pays two barrier syncs per parallel pass, to do exactly the
/// serial work the calling thread could have done inline. At 0 it spawns
/// nothing and every pass runs inline with no barrier at all
/// (`mForceMainThread`, OgreSceneManager.cpp:171 and :4705-4717). For a scene
/// that is never drawn — the document's staging scene manager — or one that
/// draws 128x128 once and is emptied again, 1 is strictly worse than 0.
///
/// The value is deliberately not 0 and deliberately not a plausible thread
/// count, so a caller that computes a number can never land on it by accident.
constexpr unsigned kSceneMainThreadOnly = ~0u;

/// The host's sink for the engine's OWN log records (SESSION_LOG_SPEC F3-B).
/// `level` is 0 for ordinary messages and 1 for LML_CRITICAL. See
/// Engine::setLogSink for the three rules an implementation must obey — it is
/// called under Ogre's log mutex, on whatever thread logged, and must never
/// call back into the engine.
using EngineLogSink = std::function<void(int level, const std::string &message)>;

/// Everything the engine needs to start. All paths are resolved by the HOST at
/// runtime (next to the executable, an env override, or a compile-time default).
/// Nothing in the engine is baked to a build-machine path.
struct EngineConfig {
    Backend     backend = Backend::Vulkan;
    /// HEADLESS: boot the backend's NULL render system instead of `backend`
    /// (SPECS/SCENEGRAPH_SPEC.md §3b, v2). Everything that is not pixels works
    /// exactly as it does on a real device — scenes, the document scene graph,
    /// nodes, meshes, materials, queries, transforms — and NOTHING renders:
    ///
    ///   * no display, no GPU and no driver are needed or opened (this is the
    ///     one mode that runs with DISPLAY unset);
    ///   * the render system creates its own 1x1 surfaceless window at boot,
    ///     which is what satisfies Ogre's window-before-SceneManager rule, so
    ///     the engine never makes a window of its own;
    ///   * createView() AND createOffscreenView() REFUSE (lastError() says so):
    ///     a NULL-render-system texture has no contents, and returning a View
    ///     whose readPixels answers uninitialised memory would be worse than a
    ///     clean "no";
    ///   * renderOneFrame() is legal and does nothing — with no View there is
    ///     nothing to draw into.
    ///
    /// ONE RENDER SYSTEM PER PROCESS: `Ogre::Root` is a singleton and its
    /// render system is chosen once, at boot. A process is EITHER headless OR
    /// rendering; there is no mixed mode and no way to upgrade one into the
    /// other. Hosts pick at startup (Studio: `--headless` / `--dump-api-docs`
    /// and the document-only suites).
    ///
    /// Also forces the persistent shader cache OFF: nothing compiles here, and
    /// a cache written under the NULL system must never be read back by a real
    /// one (the fingerprint does not name the render system).
    bool        headless = false;
    /// Directory holding the render-system plugins (RenderSystem_Vulkan.so ...).
    std::string pluginDir;
    /// Directory that CONTAINS the `Hlms/` folder (Hlms/Common, Hlms/Pbs, Hlms/Unlit).
    /// These shader templates are required at runtime, not optional sample data.
    std::string hlmsMediaDir;
    /// Log file path; empty means the backend's default name in the working directory.
    std::string logFile = "jahshaka-ogre.log";
    /// Optional forwarding sink, installed with the log listener itself — i.e.
    /// BEFORE the plugins load and the render system initialises, which is
    /// where the boot-time criticals (a missing render-system plugin, a Vulkan
    /// validation error, an ABI complaint) actually happen. Setting it through
    /// Engine::setLogSink afterwards works too and misses exactly that window.
    EngineLogSink logSink;
    /// Initial MSAA sample count for ON-SCREEN views (1 = off; 2/4/8 typical).
    /// Offscreen views (thumbnails, previews, tests) always start at 1 so their
    /// pixel readbacks stay exact — raise per view with View::setSampleCount.
    /// The driver may clamp; View::sampleCount() reports what was achieved.
    unsigned sampleCount = 1;
    /// Build a dedicated, de-duplicated, position-only vertex buffer for each
    /// mesh's shadow-map pass instead of re-streaming the full vertex. Shadow
    /// passes then read ~4x less vertex bandwidth, at the cost of extra VRAM per
    /// mesh and a GPU->CPU readback while the mesh is being created (import
    /// latency, never frame time). Process-wide and consumed at mesh-build time,
    /// so it cannot honestly be a per-scene value — a mesh built while it was on
    /// keeps its optimized buffers. Change at runtime with
    /// Engine::setShadowMeshOptimization(); it affects meshes built afterwards.
    bool optimizeShadowMeshes = true;
    /// Host's display connection; required only for on-screen Views (see above).
    NativeDisplayHandle display = 0;
    /// Vertical sync for ON-SCREEN views, as they are created (fps audit F1).
    /// True is what every window did unconditionally before this field existed.
    /// False asks for an immediate, tearing present mode — the "unlimited"
    /// pacing mode. Change it later with Engine::setVsync(), which documents
    /// what it costs and what it means; this only decides where windows start.
    bool vsync = true;

    // ---- Persistent shader cache (SHADER_CACHE_SPEC.md) ----
    /// Directory the backend may persist compiled-shader artifacts in. EMPTY =
    /// the cache is off: nothing is read, nothing is written, and every launch
    /// recompiles from scratch (the behaviour before the cache existed). The
    /// directory is DERIVED DATA — deleting it costs one slow launch and never
    /// anything a user could miss.
    std::string shaderCacheDir;
    /// The HOST's contribution to the cache fingerprint: its own build identity
    /// (app version + commit, and anything else that changes which Hlms
    /// properties the host asks for). Any change to this string invalidates the
    /// whole cache directory, which is the point — the application's C++ decides
    /// what shaders exist, and no hash inside the engine can see that.
    std::string appBuildId;
};

/// The device the engine is actually running on (SESSION_LOG_SPEC §4).
///
/// Every field already existed engine-side — the shader cache's fingerprint has
/// read vendor/device/driver out of RenderSystemCapabilities since it shipped —
/// but none of it was on the boundary, so a session log could not say which GPU
/// produced a picture. Ogre-free by construction: plain strings, filled by the
/// one TU that may include Ogre.
///
/// `apiVersion` is a SCRAPE, not an accessor: RenderSystemCapabilities carries
/// no API version, and the Vulkan render system only ever states it as a log
/// line ("Vulkan: API Version: X.Y.Z (0x...)"). The engine's log listener
/// captures it — the same "the verdict exists only as a log line" pattern the
/// shader cache's pipelineCacheReason already uses. Empty on a backend that
/// does not say, which is not an error.
struct DeviceInfo {
    std::string renderSystem;    ///< "Vulkan Rendering Subsystem", "NULL Rendering Subsystem", ...
    std::string vendor;          ///< RenderSystemCapabilities::getVendor()
    std::string deviceName;      ///< getDeviceName()
    std::string driverVersion;   ///< getDriverVersion().toString()
    std::string apiVersion;      ///< scraped; empty when the backend never says
};

/// What the persistent shader cache did this run, and what is on disk
/// (SHADER_CACHE_SPEC.md §4.5). `app.shaderCache()` is this struct.
struct ShaderCacheStats {
    /// False when EngineConfig::shaderCacheDir was empty — every other field is
    /// then either zero or still meaningful for the CURRENT RUN (compiled/loaded
    /// count shaders regardless of whether anything is persisted).
    bool               enabled = false;
    /// The resolved directory, whether or not it exists yet.
    std::string        dir;
    /// The composite key (§4.2) as printable hex. A cache written under a
    /// different fingerprint is deleted, never read.
    std::string        fingerprint;
    unsigned long long sizeBytes = 0;
    unsigned           files = 0;

    // ---- what was loaded, per layer ----
    bool     pipelineCacheLoaded = false;   ///< VkPipelineCache blob accepted by the driver
    /// WHY pipelineCacheLoaded reads the way it does — the honest per-layer
    /// report the caching audit (F7) asked for. `loadPipelineCache` is void and
    /// the driver's verdict exists only as a log line, so this is scraped from
    /// the render system's own sentences and is the difference between "there
    /// was no blob" and "the driver threw yours away":
    ///   "absent"          nothing on disk to offer
    ///   "accepted"        loaded, N bytes
    ///   "outdated"        header mismatch (device/driver/UUID/hash) — the
    ///                     normal cost of a driver update
    ///   "rejected"        vkCreatePipelineCache refused it
    ///   "silent"          the render system said nothing at all: the PowerVR
    ///                     broken-pipeline-cache workaround makes load and save
    ///                     no-ops (OgreVulkanDevice.cpp), and every launch on
    ///                     such a device pays full PSO creation
    std::string pipelineCacheReason = "absent";
    bool     microcodeLoaded = false;       ///< SPIR-V microcode map read back
    unsigned microcodeEntries = 0;          ///< entries in the live microcode map
    unsigned hlmsCachesLoaded = 0;          ///< Hlms disk caches applied (0..2: PBS, Unlit)

    // ---- shader accounting for THIS process ----
    /// Shaders the compiler actually built this run (GLSL -> SPIR-V).
    unsigned  compiledThisRun = 0;
    /// Shaders served straight out of the microcode cache — glslang never ran.
    /// Counts IN-PROCESS hits too (two shaders generated from byte-identical
    /// source share one microcode entry; the SMAA materials do it three times
    /// every launch), so a genuinely cold run reports a small non-zero value.
    /// "Did the disk cache work" is answered by microcodeLoaded, not by this.
    unsigned  loadedThisRun = 0;
    /// How many shaders the last saved run needed in total. 0 = unknown (no
    /// cache has ever been written). The startup progress counter's denominator.
    unsigned  expectedShaders = 0;
    /// Wall-clock of the last successful save, ms since the Unix epoch; 0 = never.
    long long lastSavedUnixMs = 0;
};

/// ONE LDR image filter — a "look" (POST_LOOKS_SPEC.md §4).
///
/// The looks stage is a stack of full-screen quads that runs at the very end of
/// the chain, AFTER tonemapping and AFTER SMAA (§4.2 and D5: SMAA is LDR edge
/// detection, so a look that CREATES edges — posterize bands, sharpen halos —
/// must not be smeared by it, and a look that WARPS the image must not have its
/// warped edges anti-aliased instead of the scene's). Every look is a colour
/// transform of the finished picture: nothing here can see depth, normals or
/// the scene at all.
enum class LookKind {
    /// Luma-weighted desaturation. p[0] = amount (0 = identity, 1 = grey).
    Desaturate = 0,
    /// Refraction through rippled glass: a procedural normal field offsets the
    /// UV. p[0] = amount (0 = identity), p[1] = scale (ripple frequency).
    GlassWarp,
    /// Zoom blur about a centre. p[0] = amount, p[1] = centre x, p[2] = centre
    /// y (both 0..1 in UV), p[3] = falloff exponent.
    RadialBlur,
    /// Sepia + dirt + flicker + frame jitter, all driven by `time`.
    /// p[0] = amount, p[1] = flicker, p[2] = dirt, p[3] = jitter.
    OldMovie,
    /// Colour quantization. p[0] = amount, p[1] = levels, p[2] = gamma.
    Posterize,
    /// 3x3 unsharp mask. p[0] = amount.
    Sharpen,
    /// The film grade (the unbuilt CAMERA_LENS_SPEC §6 P5, absorbed here):
    /// p[0] = amount, p[1] = saturation, p[2] = contrast, p[3] = vignette,
    /// p[4..6] = tint rgb.
    FilmGrade,
    /// Not a look: the count, for range checks on the host side.
    Count
};

/// One entry of the ordered looks stack. The PARAMETERS are uniforms pushed per
/// view every frame (chain::applyViewGlobals), so scrubbing one is free; the
/// KIND SEQUENCE is compositor shape and a change to it rebuilds the workspace
/// (ChainDesc::sameShape) — the same split hdr/exposure has had since phase 3.
///
/// A kind appears AT MOST ONCE in a stack. Every look's parameters live on ONE
/// process-global material (Ogre's MaterialManager is a singleton), so a second
/// instance of the same look would be handed the first one's numbers; the host
/// registry enforces the rule and the engine simply takes the first (§7 R2).
struct LookDesc {
    LookKind kind = LookKind::Desaturate;
    /// Meaning is per kind, documented on LookKind. p[0] is ALWAYS the amount,
    /// and every look is an exact identity at p[0] == 0 — that is what makes
    /// "a look at zero is byte-identical to no look" assertable.
    float p[8] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    bool operator==(const LookDesc &o) const {
        if (kind != o.kind) return false;
        for (int i = 0; i < 8; ++i) if (p[i] != o.p[i]) return false;
        return true;
    }
    bool operator!=(const LookDesc &o) const { return !(*this == o); }
};

/// The post-processing chain for a View (POST_CHAIN_SPEC.md).
///
/// Everything here is OFF by default, and every field is IGNORED on an offscreen
/// View: thumbnails, material previews, asset viewers and every pixel suite go
/// through createOffscreenView, and their exact colours are the contract that
/// makes them testable. Screenshots that WANT the chain ask for an on-screen
/// view or opt in explicitly.
///
/// The enable flags are per View. The TUNING (exposure, bloom threshold, AO
/// power and radius, SMAA preset) is process-global inside the backend — Ogre's
/// HDR/SSAO/SMAA materials are MaterialManager singletons — so the engine pushes
/// the values of the primary on-screen view and every other view lives with
/// them. Documented rather than hidden: it is a property of the upstream
/// implementation, not a choice.
struct PostFxDesc {
    /// Render the scene into a floating-point target and tonemap it (filmic,
    /// Hable/Uncharted2) with automatic exposure. The prerequisite for bloom.
    bool  hdr = false;
    /// Auto-exposure midpoint and the window it may adapt within. NOT stops:
    /// the value is used as e^(exposure - 2), so +0.69 is one doubling.
    float exposure = 0.0f;
    float exposureMin = -2.5f;
    float exposureMax = 2.5f;
    /// Highlight bloom. Rides the HDR node's fixed 256x256 blur chain, so it is
    /// resolution-independent and nearly free — but it needs `hdr`.
    bool  bloom = false;
    /// Where the bright pass starts, in the tonemapper's units. High values read
    /// as highlight bloom; low values as a haze filter.
    float bloomThreshold = 5.0f;
    /// The WIDTH of the bright pass's ramp, above `bloomThreshold`: below the
    /// threshold nothing blooms, above threshold+knee everything does, and in
    /// between the contribution ramps up. The backend's own control is two
    /// ABSOLUTE thresholds (min, full); a width is the same control expressed
    /// so it CANNOT INVERT — the backend clamps `full <= min` back up, and a
    /// second absolute row would let the panel offer a state the renderer
    /// silently refuses. 2.0 is what the caller hard-coded before this row
    /// existed, so the default is byte-identical (ADDENDUM A-6).
    float bloomKnee = 2.0f;
    /// Screen-space ambient occlusion. Adds a normals G-buffer to the main pass.
    bool  ssao = false;
    /// AO buffer resolution, as a factor of the view (0.5 or 1.0). The tap count
    /// is fixed at 64 by the shader and is deliberately not exposed.
    float ssaoScale = 1.0f;
    /// Contrast of the occlusion term, and how far in world units it looks.
    float ssaoPower = 1.5f;
    float ssaoRadius = 2.0f;
    /// SMAA: -1 off, 0 Low, 1 Medium, 2 High, 3 Ultra. Runs AFTER tonemapping.
    int   smaaPreset = -1;
    /// Screen-space reflections: 0 off, 1 half-resolution rays, 2 full.
    ///
    /// The one reflection source that knows about things that MOVE. A depth +
    /// normal + roughness prepass feeds a ray march against this frame's depth
    /// buffer and the PREVIOUS frame's colour; the result is handed to HlmsPbs
    /// as the pass' `ssrTexture` and upstream's own shader lerps it into the
    /// specular environment term before planar reflections and ambient
    /// (`hlms_use_ssr`). Where the march found nothing — off screen, occluded,
    /// too rough, pointing back at the camera — the confidence is zero and the
    /// pixel keeps exactly the probe/sky answer it has today.
    ///
    /// COSTS A SECOND FULL SCENE TRAVERSAL (the prepass), which is why the
    /// World Modes table never turns it on below High.
    ///
    /// KNOWN v1 LIMITATION, and it is upstream's architecture rather than ours:
    /// while SSR is on, the main scene pass shades from the prepass G-buffer
    /// (`use_prepass`), so an ALPHA-BLENDED material — Fade or Transparent,
    /// NOT Refractive, which renders in its own later pass and is unaffected —
    /// overwrites the G-buffer normal of whatever is behind it, and that
    /// surface is then shaded with the blended material's normal. Scenes that
    /// lean on translucent panes should leave SSR off until the prepass learns
    /// a visibility split.
    int   ssr = 0;
    /// How far a reflection ray may travel, in world units. Beyond this the
    /// march gives up and the pixel falls back to the probe/sky reflection.
    /// Scene-scale dependent: the default suits a room, not a landscape.
    float ssrMaxDistance = 25.0f;
    /// How thick the depth buffer's surfaces are assumed to be, in world units.
    /// A depth buffer records a surface's FRONT and nothing else, so a crossing
    /// is only accepted when the ray passed within this much of it — too small
    /// and reflections drop out behind objects, too large and they smear.
    float ssrThickness = 0.5f;
    /// Above this roughness a surface shows no screen-space reflection at all
    /// (with a ramp starting at half the value). V1 has no roughness-varying
    /// blur, so the cutoff is what keeps a matte floor from showing a sharp
    /// mirror image; raising it without a blur chain looks wrong, not better.
    float ssrRoughnessCutoff = 0.35f;
    /// A straight multiplier on the composite confidence. 1.0 is physical —
    /// the reflection replaces the probe answer where the march is confident.
    float ssrIntensity = 1.0f;
    /// Re-render refractive materials (alphaMode Refractive) in a second pass
    /// that samples the opaque result. Costs nothing when no material is.
    bool  refractions = false;

    /// THE DISTORTION PASS (POST_LOOKS_SPEC.md §5.3). Objects whose material's
    /// shading model is Distortion render into their own RG target through
    /// their own render queue, and a quad then warps the scene image by what
    /// they wrote. Like `refractions` this is resolved by the HOST from an
    /// auto/off row against whether the scene actually holds such a material,
    /// so a scene without one pays nothing: no target, no passes, and a graph
    /// identical to one built before this existed.
    ///
    /// It runs in LINEAR HDR, before the SSR history copy, SSAO and the
    /// tonemap: heat haze is a phenomenon in front of the LENS, so bloom and
    /// exposure should see the warped radiance and the anti-aliasing should
    /// clean the warped edges.
    bool  distortion = false;
    /// A global multiplier on every distortion material's own strength. 0 is
    /// inert and produces a byte-identical frame (the warp offset is exactly
    /// zero, so the compose quad's fetch lands on the source texel).
    float distortionStrength = 1.0f;

    /// THE SECONDARY-SURFACE TONEMAP (owner report 2026-09-07, fix wave item 6).
    ///
    /// The problem: everything that is NOT the main viewport — thumbnails,
    /// material and asset previews, screenshots, the PiP inset — renders the
    /// scene's raw linear radiance straight into an 8-bit target. A world the
    /// viewport grades filmically therefore photographs BLOWN OUT: a bright
    /// light clips to flat white in the thumbnail while the viewport, which
    /// tonemaps, shows the highlight rolled off.
    ///
    /// The fix cannot simply be `hdr = true`, because Ogre's HDR node is
    /// AUTO-exposed: it reduces the frame's luminance through four downscales
    /// into a 1x1 texture and then ADAPTS towards it over frames, weighted by
    /// wall-clock time. On a surface that renders two frames and reads them
    /// back, "the exposure it happened to reach" is not a value anybody can
    /// assert, and the material parameters it rides on are PROCESS-GLOBAL.
    ///
    /// `tonemapFixed` is the deterministic form: the same filmic curve, the
    /// same node, but the whole luminance-reduction chain is REPLACED by a
    /// per-frame clear of the 1x1 exposure texture to a constant derived from
    /// `exposure` alone. Same picture for the same scene, every time, on any
    /// machine — and cheaper than the auto path (five quads and four textures
    /// fewer). Ignored unless `hdr`.
    bool  tonemapFixed = false;

    /// THE LOOKS STACK (POST_LOOKS_SPEC.md §4), in FRAME ORDER: entry 0 runs
    /// first, on the tonemapped and anti-aliased image, and the last one writes
    /// the window. Empty is the default and costs exactly nothing — the stage
    /// is absent from the compositor graph, not disabled inside it, so a scene
    /// with no looks renders byte-for-byte what it rendered before this
    /// existed.
    ///
    /// The KIND SEQUENCE is shape (adding, removing or reordering rebuilds the
    /// workspace); the parameters are per-view uniforms and are free to scrub.
    /// A kind must appear at most once — see LookDesc.
    std::vector<LookDesc> looks;

    /// THE offscreen opt-in. Offscreen Views ignore every flag above unless this
    /// is set, because their exact colours are what thumbnails, previews and the
    /// pixel suites assert. Two callers set it, both deliberately: a screenshot
    /// that asked to look like the viewport (`screenshot({postFx:true})`), and
    /// the engine suite, which is the only way to pixel-test the chain at all.
    bool  allowOffscreen = false;

    bool operator==(const PostFxDesc &o) const {
        return hdr == o.hdr && exposure == o.exposure && exposureMin == o.exposureMin &&
               exposureMax == o.exposureMax && bloom == o.bloom &&
               bloomThreshold == o.bloomThreshold && bloomKnee == o.bloomKnee &&
               ssao == o.ssao &&
               ssaoScale == o.ssaoScale && ssaoPower == o.ssaoPower &&
               ssaoRadius == o.ssaoRadius && smaaPreset == o.smaaPreset &&
               ssr == o.ssr && ssrMaxDistance == o.ssrMaxDistance &&
               ssrThickness == o.ssrThickness &&
               ssrRoughnessCutoff == o.ssrRoughnessCutoff &&
               ssrIntensity == o.ssrIntensity &&
               refractions == o.refractions &&
               distortion == o.distortion &&
               distortionStrength == o.distortionStrength &&
               tonemapFixed == o.tonemapFixed &&
               looks == o.looks &&
               allowOffscreen == o.allowOffscreen;
    }
    bool operator!=(const PostFxDesc &o) const { return !(*this == o); }
};

/// What the renderer measured this frame (STATS_OVERLAY_SPEC.md §4).
/// A POD, exactly like ShaderCacheStats — `app.renderStats()` is this struct.
///
/// PROCESS-WIDE, not per view. There is one Root and one render loop, so the
/// timings describe the loop; the geometry counters are the FRAME's running
/// totals at the end of the last camera's pass (OgreSceneManager::_renderPhase02
/// snapshots them per camera), which is why they are labelled "frame" and never
/// "this view". With two on-screen views the second view's numbers include the
/// first's — an approximation upstream makes, not one this boundary adds.
///
/// THE HONESTY NOTE that belongs beside every FPS number in this application:
/// the loop is a 16 ms QTimer (EngineRenderDriver), so `fps` measures the TIMER,
/// not the renderer. A scene that got twice as expensive but still fits inside
/// 16 ms reads the same ~62. The number that diagnoses anything is the host's
/// work-per-tick (app.frameStats().workMs), not this one.
struct RenderStats {
    /// True when the backend was recording geometry counters for the frames
    /// these numbers describe. Reading renderStats() turns recording ON (it is
    /// off by default in Ogre and costs integer adds per draw call), so the
    /// FIRST read reports false with zeroed counters and every read after a
    /// rendered frame reports true. Timings are valid either way.
    bool               metricsRecording = false;

    // ---- timing: Ogre::FrameStats, fed by Root::renderOneFrame ----
    double fps = 0.0;         ///< rolling average FPS — the number to display
    double frameMs = 0.0;     ///< rolling average frame time, ms
    double lastMs = 0.0;      ///< the latest inter-sample delta, ms (noisy)
    double p95Ms = 0.0;       ///< 95th percentile frame time, ms (0.4 fps buckets)
    double p99Ms = 0.0;       ///< 99th percentile frame time, ms
    double bestMs = 0.0;      ///< best frame since the counters were reset
    double worstMs = 0.0;     ///< worst frame since the counters were reset

    // ---- geometry: Ogre::RenderingMetrics, reset per frame ----
    unsigned long long draws = 0;
    unsigned long long batches = 0;
    unsigned long long triangles = 0;
    unsigned long long vertices = 0;
    unsigned long long instances = 0;

    /// PSOs the LAST frame gave up on because it ran out of its compile budget
    /// (SPECS/THREADING_ADOPTION_SPEC.md P4(b), decision D-E(1)). Objects using
    /// an incomplete PSO do not appear that frame and are resubmitted for the
    /// next one.
    ///
    /// ALWAYS 0 IN THIS ENGINE, AND THAT IS THE POINT. The budget
    /// (RenderSystem::setPsoRequestsTimeout) is left at Ogre's default of 0,
    /// i.e. off, deliberately: upstream's own documentation warns that
    /// "techniques that rely on running a shader once (e.g. to fill a texture)
    /// may end up uninitialized", which is every thumbnail, IBL bake, VCT
    /// voxelization and offscreen pixel suite we have — and the knob is
    /// process-wide while the only thing worth protecting is one on-screen view.
    /// The counter costs one read, so it is reported anyway: a non-zero value
    /// means somebody turned the deadline on, and would be the first thing to
    /// look at if thumbnails ever came back half-drawn.
    unsigned           incompletePsoRequests = 0;

    /// FORWARD+ LIGHT CENSUS (LIGHTING_FIX fix 8 / F-F2) — the WORST case
    /// across the live scenes, because a cell overflow belongs to one scene and
    /// this struct is process-wide.
    ///
    /// Forward+ bins lights into screen-space cells and DROPS SILENTLY once a
    /// cell is full: `if( numLightsInCell->lightCount[0] < mLightsPerCell )`
    /// with no else, three times over (OgreForwardClustered.cpp:479/618/759).
    /// Nothing counts the drops and nothing exposes the per-cell counts, so an
    /// exact "lights lost this frame" cannot be reported without patching Ogre
    /// — deliberately not done for a diagnostic.
    ///
    /// What these three DO say, exactly: `forwardPlusLights` is how many lights
    /// the busiest scene puts through the clustered list at all (everything
    /// except directionals, which ride the pass buffer); `forwardPlusBudget` is
    /// the per-cell capacity they compete for. `forwardPlusOverBudget` is the
    /// excess, and it is a NECESSARY condition for a drop, not a sufficient
    /// one: zero PROVES no light was dropped anywhere, non-zero means a cell
    /// that saw every light would have dropped that many and the scene is worth
    /// looking at.
    unsigned           forwardPlusLights = 0;
    unsigned           forwardPlusBudget = 0;
    unsigned           forwardPlusOverBudget = 0;
};

/// One SHADOW-MAP SLOT, as the renderer currently holds it
/// (`world.shadowStatus()`). A slot is a LIGHT's place in the atlas, not a
/// texture rectangle: slot 0 is the directional light and owns all three PSSM
/// splits, and slots 1..N are the focused point/spot maps, one light each.
/// That is Ogre's own bookkeeping (CompositorShadowNode::getShadowCastingLights
/// is indexed by light, not by map), and reporting anything else here would
/// mean inventing a second numbering the engine does not use.
struct ShadowMapInfo {
    unsigned slot = 0;        ///< 0 = the directional/PSSM slot, 1..N the focused maps
    NodeId   node = 0;        ///< the light occupying it, 0 when the slot is empty or foreign
    bool     isStatic = false;///< tied to that light with a static map (LightDesc::shadowStatic)
    bool     dirty = false;   ///< a static map scheduled to re-render on the next frame
    bool     pssm = false;    ///< the directional slot (three splits) rather than a focused map
};

/// WHAT THE SHADOW ATLAS ACTUALLY IS, as opposed to what was asked for
/// (SPECS/SHADOW_TOOLING_SPEC.md §7) — the shape `GiStatus` established: a
/// readback of what the renderer ACHIEVED, cheap enough to poll, and the only
/// way anything (a panel, a test, a script) can tell "this light has no shadow
/// map" from "this light casts no shadow".
///
/// PROCESS-WIDE, like the resolution and the filter: there is one atlas.
struct ShadowStatus {
    /// False when there is no atlas to describe — a headless engine, or before
    /// the first view exists. Every number below is then zero.
    bool     live = false;
    unsigned resolution = 0;     ///< the base size the layout derives from
    unsigned maps = 0;           ///< TEXTURE rectangles in the atlas: pssmSplits + focusedMaps
    unsigned pssmSplits = 0;     ///< always 3 here: one directional light, three splits
    unsigned focusedMaps = 0;    ///< point/spot maps the atlas has room for
    /// LIGHT slots — 1 (the directional/PSSM set) + focusedMaps, and the length
    /// of `mapped`. Not the same number as `maps`: three of the rectangles
    /// belong to one light.
    unsigned lightSlots = 0;
    /// Shadow-casting point/spot lights in the scenes being drawn — the demand
    /// the count is derived from. `casters > focusedMaps` is the exceeded case.
    unsigned casters = 0;
    unsigned budget = 0;         ///< the EFFECTIVE ceiling (resolution-capped)
    unsigned requestedBudget = 0;///< what the host asked for before the cap
    unsigned atlasWidth = 0, atlasHeight = 0;
    /// The atlas texture's own bytes (D32). NOT included: the point-light cube
    /// scratch (1024^2 x 6 R32F + depth, ~48 MB), which is allocated once for
    /// any point caster and does not grow with the map count.
    unsigned long long atlasBytes = 0;
    /// Every light SLOT the live shadow node holds, in slot order.
    std::vector<ShadowMapInfo> mapped;
    /// Shadow-casting point/spot lights with NO map this frame — the lights
    /// whose shadows are silently missing. Empty is the healthy state.
    std::vector<NodeId> unmapped;
    /// Shadow-node passes the last frame executed, and how many of those were a
    /// static map re-rendering. A static map that never dirties contributes
    /// zero: that is what "renders once" means, measurably.
    unsigned shadowPassesLastFrame = 0;
    unsigned staticMapRendersLastFrame = 0;
};

/// A CENSUS of everything alive behind the boundary (fps audit F11).
/// A POD, exactly like RenderStats — `app.engineObjects()` is this struct.
///
/// WHY IT EXISTS. RenderStats says what a frame COST; nothing said what the
/// renderer was HOLDING. A leak on this side of the boundary — a view that is
/// created per readback and never destroyed, a mesh record kept after the
/// document node died, a datablock per material push — is invisible in pixels,
/// invisible in the document, and shows up only as a frame that costs a little
/// more every minute. These counts are flat in a scene nobody is editing, so
/// "sample 2 equals sample 6" is a contract a test can assert without a
/// per-machine baseline (tests/perf/epic_steady_state.js).
///
/// SCOPE. `views` and `scenes` are the engine's own vectors. `nodes`, `meshes`,
/// `materials` and `textures` are the per-Scene registries SUMMED over every
/// live scene — they are ids the boundary handed out and still honours, not
/// Ogre objects. `datablocks` is the one PROCESS-WIDE number: HlmsManager keeps
/// one datablock map per Hlms type for the whole Root, so it counts across
/// scenes by construction (and includes the backend's own defaults, which is
/// why only its DELTA means anything).
struct ObjectCounts {
    unsigned views = 0;         ///< live View objects (on-screen + offscreen)
    unsigned enabledViews = 0;  ///< of those, the ones renderOneFrame draws
    unsigned scenes = 0;        ///< live Scene objects
    /// Of those, how many the LAST frame updated — the scenes an enabled View
    /// draws (THREADING_ADOPTION_SPEC.md P3). The frame loop walks exactly
    /// these; every other scene manager in the process, including the
    /// document's staging ones, is skipped. `updatedScenes < scenes` in the
    /// editor is the NORMAL, wanted state; `updatedScenes == scenes` with
    /// several preview pages alive means the gate stopped working.
    unsigned updatedScenes = 0;
    /// SceneManagers the engine holds that are NOT Scenes: the document's
    /// staging manager (SPECS/SCENEGRAPH_SPEC.md D2), where every node that is
    /// not in a rendered scene lives — everything an importer builds, everything
    /// the undo stack holds, every document that has not met a SceneMirror.
    ///
    /// A ROW OF ITS OWN rather than part of `scenes` (THREADING_ADOPTION_SPEC.md
    /// P5): it has no View, no workspace, no worker threads and no place in the
    /// frame loop, and the census is a debugging instrument — a number that
    /// mixes two kinds of object answers no question. 0 until the host asks for
    /// documentGraphScene(), 1 after.
    unsigned stagingScenes = 0;
    unsigned nodes = 0;         ///< tracked node records, summed over scenes
    unsigned meshes = 0;        ///< tracked mesh records, summed over scenes
    unsigned materials = 0;     ///< tracked material records, summed over scenes
    unsigned textures = 0;      ///< tracked texture records, summed over scenes
    unsigned datablocks = 0;    ///< Hlms datablocks in the process (all types)
};

/// WHAT THE ENGINE IS THREADING (SPECS/THREADING_ADOPTION_SPEC.md P1).
/// `app.threading()` is this struct. It exists because the single most
/// expensive failure mode of the threading program is SILENT: an engine built
/// with a stale CMake cache keeps single-threaded shader compilation, every
/// build still succeeds, every test still passes, and the editor compiles its
/// shaders on one core for ever. This is the read-back that makes that
/// falsifiable from a script.
struct EngineThreading {
    /// `RenderSystem::supportsMultithreadedShaderCompilation()` — the backend's
    /// own answer, not our guess. False means the parallel Hlms compile queue
    /// never starts (OgreRenderQueue.cpp:388) and HlmsDiskCache::applyTo runs
    /// its serial branch whatever thread count it is given.
    bool multithreadedShaderCompilation = false;
    /// The build-time mode this engine was compiled with, decoded from the two
    /// OgreBuildSettings.h macros the CMake option sets:
    ///   0 = disabled           (BACKWARDS_COMPATIBLE_API, no TLS)
    ///   1 = compatible API     (BACKWARDS_COMPATIBLE_API + TLS, static builds)
    ///   2 = force-enabled      (no BACKWARDS_COMPATIBLE_API)
    /// 0 and 1-without-TLS are indistinguishable in the header, so a shared
    /// build reports 1 for both; what matters operationally is
    /// multithreadedShaderCompilation above.
    unsigned shaderThreadingMode = 1;
    /// Per-scene worker pools, by scene name — the threads Ogre forks culling,
    /// transforms, bounds and (after mode 2) shader compilation across. The
    /// engine's staging scene managers are not Scenes and are not listed.
    std::vector<std::pair<std::string, unsigned>> sceneWorkerThreads;
    /// The largest of those counts: the ceiling on how many threads any single
    /// pass can compile shaders on, and the count the disk cache's applyTo is
    /// given at load. 1 means "serial no matter what the flag says".
    unsigned hlmsThreads = 1;
};

/// Where a corner-anchored readout sits in a View.
enum class OverlayCorner { TopLeft, TopRight, BottomLeft, BottomRight };

/// ONE engine-drawn overlay per View (STATS_OVERLAY_SPEC.md §5.1). It covers
/// BOTH jobs the owner asked for: a stats readout in a corner, and/or a
/// full-view cover while nothing is being presented.
///
/// One desc, not two, because the cover and the readout are the same mechanism,
/// the same Ogre overlay, the same per-pass gate and the same offscreen rule.
/// The cover fill is drawn UNDER the stats text, so "stats visible while
/// loading" is free and correct — it is the frame you most want numbers for.
///
/// IGNORED ON OFFSCREEN VIEWS unless `allowOffscreen` is set, exactly like
/// PostFxDesc and enforced in the same single place (OgreView::chainDesc): the
/// cover is up during EVERY open, so a leak into offscreen views would put a
/// flat grey panel over every thumbnail and every pixel suite in the tree.
///
/// CONSTRAINT, documented rather than hidden: Ogre's OverlayManager is a
/// PROCESS-WIDE singleton with one overlay set, so two on-screen Views cannot
/// show DIFFERENT text at the same time. The application's shape saves us — all
/// its on-screen views live on different pages, so at most one is enabled at a
/// time, and the engine composes the overlay from the enabled view's desc once
/// per frame. If that invariant is ever broken (a tear-off viewport, a second
/// window, a VR mirror) this becomes a real limitation.
struct ViewOverlayDesc {
    // ---- stats readout ----
    bool          stats = false;
    OverlayCorner corner = OverlayCorner::TopLeft;
    /// Text size multiplier. Snapped to sensible steps by the backend; HiDPI is
    /// NOT solved anywhere in this engine, so on a Retina panel this is the
    /// only handle there is.
    float         scale = 1.0f;
    Colour        colour { 1.0f, 1.0f, 1.0f, 1.0f };
    /// The lines the HOST wants drawn, top to bottom. Empty = the engine draws
    /// its own one-line RenderStats summary, which is what makes this boundary
    /// testable with no host at all.
    ///
    /// The host supplies these because the most useful number — work per tick —
    /// is the host's, not Ogre's. Rate-limit the pushes host-side (4-10 Hz): a
    /// number that changes 62 times a second is unreadable anyway.
    std::vector<std::string> lines;

    // ---- the cover (the ported ViewportCover states) ----
    enum class Cover { None, Loading, NoScene };
    Cover       cover = Cover::None;
    std::string coverTitle;      ///< "Loading world…" / "No world open"
    std::string coverSubtitle;   ///< the world's name, or the "Open or create…" hint
    /// The fill grey from viewportcover.cpp:15, kept to the pixel.
    Colour      coverFill { 44 / 255.0f, 46 / 255.0f, 52 / 255.0f, 1.0f };

    /// THE offscreen opt-in — same meaning and same word as
    /// PostFxDesc::allowOffscreen. Only the engine suite sets it.
    bool allowOffscreen = false;

    /// THE SHADOW-ATLAS INSPECTOR (SPECS/SHADOW_TOOLING_SPEC.md §4.4): a strip
    /// of thumbnails along the bottom of the view, one per shadow map, showing
    /// what the renderer actually rasterised into each rectangle of the atlas,
    /// captioned with the light it belongs to and whether its map is static.
    ///
    /// A DIAGNOSTIC, not a feature: never persisted, off in every offscreen
    /// view unless allowOffscreen, and process-wide like the rest of this HUD —
    /// it shows the atlas the PRIMARY view rendered, so a second on-screen view
    /// displays the same tiles.
    bool shadowAtlas = false;

    /// True when this desc asks for anything to be drawn at all.
    bool anything() const { return stats || shadowAtlas || cover != Cover::None; }

    bool operator==(const ViewOverlayDesc &o) const {
        return stats == o.stats && corner == o.corner && scale == o.scale &&
               colour == o.colour && lines == o.lines && cover == o.cover &&
               coverTitle == o.coverTitle && coverSubtitle == o.coverSubtitle &&
               coverFill == o.coverFill && allowOffscreen == o.allowOffscreen &&
               shadowAtlas == o.shadowAtlas;
    }
    bool operator!=(const ViewOverlayDesc &o) const { return !(*this == o); }
};

/// A CPU-side RGBA8 image, used to read back an offscreen View.
struct Image {
    unsigned width = 0, height = 0;
    std::vector<unsigned char> rgba;   // width*height*4, row-major, top-left origin
    /// Pixel accessor; returns {0,0,0,0} if out of range.
    Colour at(unsigned x, unsigned y) const {
        if (x >= width || y >= height) return Colour(0, 0, 0, 0);
        const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
        return Colour(rgba[i] / 255.0f, rgba[i+1] / 255.0f, rgba[i+2] / 255.0f, rgba[i+3] / 255.0f);
    }
};

}}  // namespace jahshaka::engine
