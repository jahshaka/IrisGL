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
enum class PbrTextureSlot { Albedo, Normal, Metalness, Roughness, Emissive };

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
enum class ShadingModel {
    Lit,   ///< the metallic-roughness PBR family — everything above works
    Unlit  ///< flat colour; the constraint list above applies in full
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
    float  uvScale         = 1.0f;   ///< tiles every bound texture map (UV *= uvScale);
                                     ///< the document's PbrMaterial::textureScale
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

    /// "Is this the same material state I last pushed?" — the guard a host with
    /// a per-frame push loop needs. Exact comparison (see Colour::operator==):
    /// a tolerance here would let a dragged slider stop reaching the backend.
    bool operator==(const PbrParams &o) const {
        return albedo == o.albedo && metalness == o.metalness && roughness == o.roughness &&
               emissive == o.emissive && alphaMode == o.alphaMode && alpha == o.alpha &&
               alphaCutoff == o.alphaCutoff && twoSided == o.twoSided &&
               normalMapWeight == o.normalMapWeight && uvScale == o.uvScale &&
               refractionStrength == o.refractionStrength &&
               clearCoat == o.clearCoat && clearCoatRoughness == o.clearCoatRoughness &&
               brdf == o.brdf && receiveShadows == o.receiveShadows &&
               emissiveAsLightmap == o.emissiveAsLightmap &&
               shadingModel == o.shadingModel;
    }
    bool operator!=(const PbrParams &o) const { return !(*this == o); }
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
        DeflectorPlane  ///< bounce off an infinite plane
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

    /// THE WIDE-ASPECT FOV CLAMP (owner report 2026-09-07). `fovDegrees` is
    /// VERTICAL, so the HORIZONTAL angle it produces grows with the target's
    /// aspect: the default 45-degree explorer is 75 degrees wide at 16:9 and
    /// 118 degrees wide at 32:9 — a fisheye nobody asked for, on exactly the
    /// monitors people buy to see more of a scene.
    ///
    /// A positive value caps the HORIZONTAL angle at that many degrees: past
    /// the aspect where the cap first bites, the vertical angle is narrowed to
    /// hold it (verticalFovForHorizontalCap below). Zero — the DEFAULT — is
    /// off, which is what every AUTHORED camera gets: a scene camera's angle is
    /// a deliberate lens choice and the engine must not second-guess it. Only
    /// the two FREE cameras (the editor explorer, the player's fly camera) set
    /// it, and only they can, because only their hosts know they are free.
    float maxHorizontalFovDegrees = 0.0f;

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
               maxHorizontalFovDegrees == o.maxHorizontalFovDegrees &&
               lensShiftX == o.lensShiftX && lensShiftY == o.lensShiftY;
    }
    bool operator!=(const CameraDesc &o) const { return !(*this == o); }
};

/// The vertical angle of view that holds the HORIZONTAL angle at `hfovCapDeg`
/// for a target of `aspect` (width / height) — CameraDesc::maxHorizontalFov.
///
///     hfov = 2 * atan( tan(vfov/2) * aspect )
///     vfov = 2 * atan( tan(hfovCap/2) / aspect )
///
/// The clamp only ever NARROWS: below the aspect where the cap bites (about
/// 16:9 for a 95-degree cap on a 45-degree lens) the authored vertical angle is
/// already inside it and is returned BIT-IDENTICALLY — no arithmetic runs at
/// all, which is what lets a 16:9 pixel suite stay byte-exact. Free (a
/// non-positive cap or aspect) is likewise the identity.
///
/// A free function, in the header, deliberately: this is the whole of the
/// policy, and a suite can drive it across an aspect sweep with no engine at
/// all (tests/cameras' fov_clamp case).
inline float verticalFovForHorizontalCap(float vfovDeg, float aspect, float hfovCapDeg) {
    if (!(hfovCapDeg > 0.0f) || !(aspect > 0.0f) || !(vfovDeg > 0.0f)) return vfovDeg;
    const float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float capHalf = std::min(hfovCapDeg, 179.0f) * 0.5f * kDegToRad;
    const float haveHalf = std::min(vfovDeg, 179.0f) * 0.5f * kDegToRad;
    // The horizontal angle this vertical angle actually produces here.
    const float haveHorizontalHalf = std::atan(std::tan(haveHalf) * aspect);
    if (haveHorizontalHalf <= capHalf) return vfovDeg;   // inside the cap: untouched
    const float wantHalf = std::atan(std::tan(capHalf) / aspect);
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
///   * its colour LOADs (a Clear on Vulkan is full-target and would wipe the
///     main frame) and its depth CLEARs (Load leaves the main view's depth
///     occluding 92% of the inset — a correctness requirement, not a saving);
///   * because the colour Loads, the inset needs its OWN BACKGROUND: an unlit
///     quad pass at the rect, before the scene pass;
///   * addWorkspace's vpModifierMask defaults to 0x00, which silently makes the
///     rect inert — it is passed 0xFF;
///   * the main chain's final pass must keep MSAA samples AND resolve them
///     (chain::kMultiWorkspaceStore) or the inset destroys the frame at 4x;
///   * cameras are POOLED and destroyed AFTER the workspace that names them
///     (the other order segfaults on the next frame).
///
/// COST: a second cull and a second render of everything the inset camera sees.
/// The spike measured +0.33 ms/frame CPU on a trivial scene. It is off unless a
/// host asks for it, and the editor asks only while a camera is selected.
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

    bool operator==(const ViewPipDesc &o) const {
        return enabled == o.enabled && camera == o.camera && left == o.left && top == o.top &&
               width == o.width && height == o.height && background == o.background &&
               allowOffscreen == o.allowOffscreen;
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

    /// THE offscreen opt-in. Offscreen Views ignore every flag above unless this
    /// is set, because their exact colours are what thumbnails, previews and the
    /// pixel suites assert. Two callers set it, both deliberately: a screenshot
    /// that asked to look like the viewport (`screenshot({postFx:true})`), and
    /// the engine suite, which is the only way to pixel-test the chain at all.
    bool  allowOffscreen = false;

    bool operator==(const PostFxDesc &o) const {
        return hdr == o.hdr && exposure == o.exposure && exposureMin == o.exposureMin &&
               exposureMax == o.exposureMax && bloom == o.bloom &&
               bloomThreshold == o.bloomThreshold && ssao == o.ssao &&
               ssaoScale == o.ssaoScale && ssaoPower == o.ssaoPower &&
               ssaoRadius == o.ssaoRadius && smaaPreset == o.smaaPreset &&
               ssr == o.ssr && ssrMaxDistance == o.ssrMaxDistance &&
               ssrThickness == o.ssrThickness &&
               ssrRoughnessCutoff == o.ssrRoughnessCutoff &&
               ssrIntensity == o.ssrIntensity &&
               refractions == o.refractions &&
               tonemapFixed == o.tonemapFixed &&
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

    /// True when this desc asks for anything to be drawn at all.
    bool anything() const { return stats || cover != Cover::None; }

    bool operator==(const ViewOverlayDesc &o) const {
        return stats == o.stats && corner == o.corner && scale == o.scale &&
               colour == o.colour && lines == o.lines && cover == o.cover &&
               coverTitle == o.coverTitle && coverSubtitle == o.coverSubtitle &&
               coverFill == o.coverFill && allowOffscreen == o.allowOffscreen;
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
