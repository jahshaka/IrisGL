#pragma once
// Engine-neutral value types. NOTHING here may reference Ogre, Qt or GL.
#include <cstddef>
#include <string>
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

/// PBR texture slots. NOTE: there is deliberately NO Occlusion slot — the Ogre
/// backend (HlmsPbs) has no dedicated ambient-occlusion input, so the document's
/// occlusionMap/occlusionFactor are documented as unsupported rather than faked
/// (bake AO into the base colour map at import time if it matters).
enum class PbrTextureSlot { Albedo, Normal, Metalness, Roughness, Emissive };

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

/// Metallic-roughness PBR parameters — Jahshaka's material model, sized to what
/// the backend's PBR pipeline can honour. Emissive arrives with any intensity
/// already folded in (colour * intensity). Roughness remap bounds are applied by
/// the CALLER as a clamp before filling `roughness` — the backend has no
/// per-texel remap. Texture maps bind separately via setPbrTexture().
struct PbrParams {
    Colour albedo   = Colour(0.8f, 0.8f, 0.8f);
    float  metalness = 0.0f;
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
    float     spotAngleDegrees = 30.0f;    // outer cone
    float     spotSoftness = 0.1f;         // 0..1, inner = outer * (1 - softness)
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

/// Everything the engine needs to start. All paths are resolved by the HOST at
/// runtime (next to the executable, an env override, or a compile-time default).
/// Nothing in the engine is baked to a build-machine path.
struct EngineConfig {
    Backend     backend = Backend::Vulkan;
    /// Directory holding the render-system plugins (RenderSystem_Vulkan.so ...).
    std::string pluginDir;
    /// Directory that CONTAINS the `Hlms/` folder (Hlms/Common, Hlms/Pbs, Hlms/Unlit).
    /// These shader templates are required at runtime, not optional sample data.
    std::string hlmsMediaDir;
    /// Log file path; empty means the backend's default name in the working directory.
    std::string logFile = "jahshaka-ogre.log";
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
    /// NOT IMPLEMENTED YET — the field and the chain's plumbing exist, but the
    /// backend builds no SSR passes, so a non-zero value renders as if it were
    /// zero. World Modes declares the row unavailable for the same reason.
    int   ssr = 0;
    /// Re-render refractive materials (alphaMode Refractive) in a second pass
    /// that samples the opaque result. Costs nothing when no material is.
    bool  refractions = false;

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
               ssr == o.ssr && refractions == o.refractions &&
               allowOffscreen == o.allowOffscreen;
    }
    bool operator!=(const PostFxDesc &o) const { return !(*this == o); }
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
