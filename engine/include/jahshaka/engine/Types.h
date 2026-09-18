#pragma once
// Engine-neutral value types. NOTHING here may reference Ogre, Qt or GL.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace jahshaka { namespace engine {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    /// EXACT float equality, for the same reason Colour::operator== is exact:
    /// it answers "is this the value I already pushed?", and a tolerance would
    /// let a dragged slider stop reaching the backend.
    bool operator==(const Vec3 &o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3 &o) const { return !(*this == o); }
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
// ---- ATOM stage 1: THE LEVEL THAT STANDS IN FOR A MESH AT A GIVEN SIZE -----
//
// THE RULE, written ONCE and cited from all of its callers
// (`MeshData::lodForWorldError` below; `OgreScene::cascadeVoxelLod` in
// irisgl/engine/src/OgreGi.cpp, which spends it on Photon's cascades; and the
// engine's view LOD strategy in OgreMesh.cpp, which is the same `lower_bound`
// over the same errors done four-wide in Ogre's own SoA loop):
//
//     take the COARSEST level whose error is strictly below `allowed` — the
//     WORLD-SPACE error THIS consumer has said it can afford.
//
// ONE RULE, AND EVERY CONSUMER COMPUTES ITS OWN `allowed` FROM WHAT IT SAMPLES
// (ATOM-3, the render audit's A8). The view: a pixel budget at the LIVE camera
// and the LIVE viewport height, `pixels * (d - r) * 2 / (proj[1][1] * height)`.
// A cascade: a MEASURED fraction of its own cell — not half of it, because a
// voxel's occupancy is a binary triangle-box test and the boundary moves with
// the deviation (the fraction, its measurement and its picture evidence are at
// `kCascadeLodCellFraction` in OgreGi.cpp). A far-field proxy: a fraction of
// the distance. The CONSTANTS are the things to measure; the rule is arithmetic.
//
// `errors[i]` is level i+1's simplifier error as a LENGTH in the same units as
// `cellSize` (see MeshData::lodErrors), and the errors are non-decreasing, so
// the first level that fails the test ends the walk. The answer is 0 — the
// authored geometry — whenever even level 1 is too coarse, and whenever the
// size is not a positive finite number.
//
// WHY "BELOW THE ALLOWED ERROR" IS THE WHOLE RULE: the baked error is the worst
// distance a level's surface may sit from the authored one, so a consumer that
// cannot see a difference of `allowed` cannot see that level either. What
// `allowed` IS belongs to the consumer and to nobody else — and the two shipped
// consumers have measured it, which is the only way this number was ever going
// to be right (the old text claimed "a sample of its own resolution" for the
// voxeliser and was wrong by two orders of magnitude; see OgreGi.cpp). The
// baked error is the COMBINED position+attribute quadric error, which is >= the
// pure geometric one, so every answer here is conservative (a finer level than
// geometry alone needs).
//
// `levelsAvailable` caps the answer at the levels the consumer actually has —
// the document's index lists, or the VAOs the engine built from them.
inline size_t lodLevelForWorldError(const std::vector<float> &errors, float allowed,
                                   size_t levelsAvailable) {
    if (!(allowed > 0.0f)) return 0;
    size_t level = 0;
    for (size_t i = 0; i < errors.size() && i < levelsAvailable; ++i) {
        if (!(errors[i] < allowed)) break;    // errors are non-decreasing
        level = i + 1;
    }
    return level;
}

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

    // ---- ATOM stage 1: the automatic LOD chain (SPECS/NANITE_SPEC.md §7) ----
    //
    // Built at IMPORT by MeshBake (irisgl/import/meshbake.cpp) and carried here
    // as plain data. Both empty = today's behaviour exactly: one level, no
    // selection, nothing changes.
    //
    // `lodIndices[i]` is LEVEL i+1 — level 0 IS `indices` — and it indexes the
    // SAME vertices. That is a rule, not an accident: one vertex buffer and N
    // index buffers is what keeps every level of a mesh inside ONE draw call
    // (the VAO is matched on {opType, indexBufferVbo, indexType, vertexBuffers},
    // so a per-level vertex remap would break the auto-instancing merge).
    //
    // `lodErrors[i]` is level i+1's SIMPLIFIER error as a LENGTH IN MESH UNITS —
    // meshoptimizer's combined position + attribute (UV, normal) quadric error,
    // which is >= the pure geometric error (the second read of ATOM-1): every
    // consumer that compares it with a world-space size is CONSERVATIVE (a finer
    // level than the geometry alone would need). A true geometric bound is a
    // recorded follow-up;
    // monotonically non-decreasing. It is the currency of the whole program,
    // and every consumer spends it the same way — by stating the world-space
    // deviation IT can afford and taking the coarsest level below it:
    //   * the VIEW turns a PIXEL budget into that deviation at its own live
    //     lens and viewport height (`kLodBudgetPixels`, and the strategy in
    //     OgreMesh.cpp), and
    //   * a VOXELISER turns its own CELL into it through a measured fraction
    //     (`kCascadeLodCellFraction` in OgreGi.cpp — a binary occupancy test
    //     moves its boundary with the deviation, so the fraction is small and
    //     it was measured on the picture, not argued).
    std::vector<std::vector<unsigned>> lodIndices;
    std::vector<float>                 lodErrors;

    size_t vertexCount() const { return positions.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }
    /// Levels including level 0 — always at least 1.
    size_t lodLevelCount() const { return lodIndices.size() + 1; }
    /// The index list of a level; level 0 is `indices`. Out-of-range clamps to
    /// the coarsest level rather than reading past the end.
    const std::vector<unsigned> &lodLevelIndices(size_t level) const {
        if (level == 0 || lodIndices.empty()) return indices;
        return lodIndices[std::min(level, lodIndices.size()) - 1];
    }
    /// The COARSEST level that still stands in for this mesh when the consumer
    /// can afford a world-space deviation of `allowed` —
    /// THE RULE ITSELF IS `lodLevelForWorldError` ABOVE, stated once and shared
    /// with the engine's voxeliser (OgreScene::cascadeVoxelLod). This overload
    /// is the document-side convenience: it clamps to the levels this mesh
    /// actually carries.
    size_t lodForWorldError(float allowed) const {
        return lodLevelForWorldError(lodErrors, allowed, lodIndices.size());
    }
    bool hasSkinData() const {
        return !blendIndices.empty() && blendIndices.size() == vertexCount() * 4 &&
               blendWeights.size() == vertexCount() * 4;
    }
};

// ---- ATOM stage 1: the VIEW's allowed error -------------------------------
//
// THE VIEW'S BUDGET IS A REAL SCREEN-SPACE PIXEL ERROR, at the live lens and
// the live viewport (ATOM-3, the render audit's A1). The projection is
// clusterlod.h's, verbatim (its own comment, lines 94-97; vendored at
// irisgl/thirdparty/meshoptimizer-clusterlod/):
//
//     screen error (pixels) = error / (distance(centre, eye) - radius)
//                             * proj[1][1] * 0.5 * viewport height
//
// INVERTED, it is the world-space error a view can afford at an object:
//
//     allowed = (distance - radius) * 2 * budgetPixels / (proj[1][1] * height)
//
// and `distance - radius` is EXACTLY what Ogre's SoA LOD loop already computes
// per object, so the engine's own LOD strategy (`OgreMesh.cpp`,
// `JahWorldErrorLodStrategy`) evaluates this per PASS with that pass's camera
// and render target and compares it against the mesh's baked errors directly.
//
// WHAT THIS REPLACES, and why it was wrong: stage 1 shipped a REFERENCE
// projection — 1080 lines at a 45 degree vertical field of view — baked into
// per-mesh switch DISTANCES, so "one pixel of error" was one pixel only on a
// 1080-line window at 45 degrees. A 30 degree lens on a 1440-line window
// switched at 2.05 px, a 90 degree lens at 720 lines at 0.28, and a VR eye at
// 2376 lines got twice the error the desktop did — on the same asset, in the
// same frame. The reference constants (`LodReference`) and `lodSwitchDistance`
// are DELETED with this note; nothing derives a distance any more.
constexpr float kLodBudgetPixels = 1.0f;   ///< the budget: one pixel of the simplifier's (combined, >= geometric) error

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

/// WHAT THIS SCENE'S MOBILITY LOOKS LIKE (SPECS/REALTIME_REFLECTIONS_SPEC.md
/// §3.3, lane R1). The host resolves every node's mobility from the document
/// (drivers, parents, the user's setting) and pushes the answer with
/// Scene::setNodeMovable; these are what the engine RECORDED, and the way to
/// see that a push landed. The renderer SPENDS it (lane R2): a movable object
/// carries no GI-geometry bit, so it does not voxelize and bounces no light,
/// and a classification that crosses that edge invalidates the GI caches
/// (`mobilityRebuilds`) or stales the probe grid with reason Mobility.
struct MobilityStatus {
    size_t movableItems = 0;    ///< movable nodes carrying drawable geometry
    size_t movableLights = 0;   ///< movable nodes carrying a light
    size_t movableNodes = 0;    ///< every node the host marked movable
    /// From-scratch GI rebuilds a mobility CHANGE caused, and the reason is
    /// always the same one: an object's GI class turning on or off is a GI edge
    /// (a movable object does not voxelize), so RE-classifying a live object
    /// the room has already been lit with costs one rebuild.
    ///
    /// WHICH FLIPS COST ONE, honestly: an AUTHORING change (the user sets
    /// Movable or Static in the properties panel) on an object that is already
    /// in the scene, in a scene whose GI arm is built. A classification that
    /// arrives BEFORE the object's geometry does — which is every load and
    /// every newly added node, because the host resolves mobility in the same
    /// walk that creates the node — costs nothing at all, and neither does the
    /// play-time SOFT promotion (MobilityChange::Soft, owner decision O3).
    unsigned long long mobilityRebuilds = 0;
};

/// WHY a node's mobility changed, which decides what the renderer may spend on
/// it (REALTIME_REFLECTIONS_SPEC §3.3.3).
enum class MobilityChange : unsigned {
    /// The classification the document derived or the user set. The GI class
    /// follows it: an object becoming movable LEAVES the voxel bounce and an
    /// object becoming static JOINS it, and either edge costs one from-scratch
    /// GI rebuild if the scene is already lit (counted in mobilityRebuilds).
    Authoring = 0,
    /// THE PLAY-TIME SOFT PROMOTION (owner decision O3): something nobody
    /// marked Movable started moving while the document is playing. The render
    /// channel and every GI gather drop it from that frame, and nothing is
    /// RE-SOLVED: no voxelization, no from-scratch rebuild, no mobilityRebuilds
    /// — the voxels keep the bounce light it had where it started, as a ghost,
    /// until play stops. That is the whole point: a surprise mover must never
    /// buy the author a half-second freeze mid-play.
    ///
    /// IT DOES COST ONE PROBE-GRID CATCH-UP, and deliberately (clean-2 lane,
    /// 2026-09-13, correcting this doc rather than the code): a promoted object
    /// leaves the probe channel as well — a movable item carries kMovableBit
    /// INSTEAD OF kVisibleBit — so every probe holding its photograph is
    /// holding an object the probes no longer capture. Leaving those captures
    /// alone would freeze the mover's image into the room's reflections for the
    /// rest of play, following nothing. So the grid is staled once, with reason
    /// Mobility, and drains at the budget; that is a rate, not a hitch.
    Soft = 1
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
/// engine-side a zero length is fmod(t, 0) = NaN, so the padding is not
/// cosmetic. It is a GUARD — a clip whose keys span no time and whose file
/// declares no duration. The one-frame T-pose every Mixamo character download
/// ships arrives one frame long since smoke L10 item 2 (the document keeps the
/// file's declared duration), so it no longer reaches the pad.
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
enum class SkyMode { NoSky, Equirectangular, Cubemap, Atmosphere };   // 'None' collides with X11's macro

/// THE ANALYTIC SKY, drawn by the engine itself (SKY-GPU, owner pick 5).
///
/// It is Ogre's `AtmosphereNpr` component: a full-screen quad whose fragment
/// shader turns the camera ray into a scattering colour, evaluated on the GPU
/// every frame. It replaces a CPU "Preetham" bake that cost up to 1024x512
/// pixels of transcendental math on the UI thread per parameter change, and its
/// dials are the component's own — there is no mapping from the old ones and
/// none is owed (the model is different arithmetic, not a re-parameterisation).
///
/// THE SUN IS NOT THE COMPONENT'S. `sunDir` is pushed by the host from the
/// scene's sun light, and the component's own light/ambient link is never armed
/// (see OgreSky.cpp): the light keeps the colour and the power the user gave it.
/// The component's own sun DISC is off too — the disc is SunDisc's, one
/// mechanism over every sky type.
/// THESE FIVE DEFAULTS ARE THE CLEAR-SKY FIT (lane SKY-TUNE-1, 2026-09-14) and
/// they MUST stay equal to `iris::SkyRealistic::defaults()` in the document
/// (irisgl/document/scenegraph/scene.cpp), which carries the derivation. They
/// are written out twice because the engine's public headers may not include
/// the document's — the boundary — and a host that pushes a whole sky
/// overwrites all five anyway; what this default decides is what a DIRECT
/// engine caller (test_engine, the spikes) gets. Ogre's own shipped preset
/// (0.47 / 2.0 / 1.0) is tuned for sunsets and is what these used to be.
struct AtmosphereSky {
    /// How much atmosphere the ray travels through: the blue's depth. (0; 1]-ish.
    float density   = 0.25f;
    /// How fast the colour changes with altitude — the horizon's spread.
    float diffusion = 2.0f;
    /// The lowest the sky is drawn at; raises the horizon band in a sunset.
    float horizon   = 0.025f;
    /// The sky's own colour, before absorption. Ogre's default is a daylight blue.
    Colour skyColour { 0.334f, 0.57f, 1.0f, 1.0f };
    /// Multiplies the whole sky (HDR). 1.5 re-anchors the Sky Light's ambient to
    /// the level the tree is tuned around after the density fit dropped it to
    /// 0.66 (the model's radiance is proportional to densityCoeff).
    float skyPower  = 1.5f;
    /// THE AIR ON THE WAY TO THE SUN — the atmosphere's turbidity, and the only
    /// input to `Scene::atmosphereSunTint` (lane SKY-DENSITY-1). It is NOT a
    /// sky-look dial and it reaches AtmosphereNpr's preset nowhere: the sky's
    /// radiance is the NPR model's business (`density` above), the direct
    /// beam's extinction is Beer-Lambert physics, and one number could not
    /// serve both without each edit moving the other. 1 = a purely molecular
    /// atmosphere, 2.5 = the clear day the sky's own defaults were fitted to,
    /// 4-6 = hazy; held at or above 1 (below it the aerosol term amplifies).
    ///
    /// It is deliberately ABSENT from the comparison below, which asks "is this
    /// the same SKY?" and decides whether the backend tears the sky down,
    /// re-captures the environment and stales the probe grid. This dial changes
    /// no sky pixel and no reflection — only the colour of the direct sunlight
    /// — so it is applied on its own, like the sun disc (Scene::setSky).
    float sunHaze   = 2.5f;
    /// Unit vector FROM the scene TOWARDS the sun, in world space — the scene's
    /// sun light's direction, reversed, pushed by the host. With `hasSun` false
    /// the sky is evaluated with the sun straight overhead at its lowest time
    /// of day, which is this model's night.
    float sunDir[3] = { 0.0f, 1.0f, 0.0f };
    bool  hasSun    = false;

    /// THE COST, stated where the mode is chosen: an analytic sky is drawn by a
    /// component the backend has to REGISTER on the scene (its quad's per-camera
    /// rays come from that registration), and registering it puts `hlms_fog`
    /// into every PBS pass hash for as long as the sky is bound — a second
    /// permutation set for the scene's materials and, on a cold shader cache,
    /// a compile hitch the first time the sky is switched on. The fog block
    /// itself is an exact identity while the World fog is off (density 0), so
    /// it costs shader COMPILES and a few ALU, never a pixel. MEASURED on a
    /// floor + a metal sphere with the fog OFF: 100 shader compiles with a
    /// colour sky, 104 after switching to the analytic one — four permutations
    /// and one hitch, once, warm-cached afterwards. With the fog ON (which is
    /// every scene this engine ships) the property is already in the hash and
    /// the analytic sky adds nothing at all: 96 either way.
    bool operator==(const AtmosphereSky &o) const {
        return density == o.density && diffusion == o.diffusion && horizon == o.horizon &&
               skyColour.r == o.skyColour.r && skyColour.g == o.skyColour.g &&
               skyColour.b == o.skyColour.b && skyPower == o.skyPower &&
               hasSun == o.hasSun && sunDir[0] == o.sunDir[0] && sunDir[1] == o.sunDir[1] &&
               sunDir[2] == o.sunDir[2];
    }
    bool operator!=(const AtmosphereSky &o) const { return !(*this == o); }
};

/// A SCENE'S WHOLE SKY, as one value (ENGINEERING_DEBT_SPEC.md item 4).
///
/// It replaces the three entry points this boundary used to have —
/// `setSky(SkyMode, TextureId)`, `setSkyCubemap(faces[6])` and
/// `setSkyReflection(faces[6])` — which between them made the host write the
/// dispatch, the ordering ("sky first, then reflections") and the
/// already-pushed bookkeeping that the backend is in a better position to own.
/// Scene::setSky(const SkyDesc &) takes the whole description and is
/// IDEMPOTENT: pushing a description equal to the live one does nothing at all
/// — no re-upload, no cube rebuild, no IBL reconvolution — so a host may push
/// it every frame and let `operator==` be the change guard.
///
/// WHAT IS NOT HERE. The flat "single colour sky" is not a sky at all in the
/// backend: it is the VIEW's background (View::setBackground), and several
/// views of one scene legitimately clear to different colours (an opaque
/// editor, a transparent thumbnail). It stays a view property on purpose.
/// The gradient and Preetham "realistic" skies are not here either: both are
/// CPU bakes of DOCUMENT parameters into an equirectangular image, and the
/// image is what this boundary consumes — see SceneMirror::applySky, which
/// owns those bakes because they need an image decoder and this layer has
/// none (no Qt, no image formats — Types.h's first line).
/// THE SUN DISC (SKY_LIGHT_SPEC.md §3) — the bright disc drawn in the sky where
/// the scene's sun light points, as part of the sky description.
///
/// It is the SUN'S, not the sky's, and there is exactly ONE of it. It used to
/// be a term baked into the analytic sky's texels, which made it a property of
/// one sky type, at the bake's resolution, in a place no capture mask could
/// exclude it from. Here it is a quad the backend draws over WHATEVER sky is
/// bound — a colour, a gradient, a Preetham bake, an equirect photograph — with
/// its own visibility channel, so "not in the reflection probes" is expressible.
///
/// `colour` is the disc's RADIANCE, already multiplied by whatever the host
/// wants (the sun light's colour, its intensity, an overdrive constant); the
/// backend writes it additively and does not scale it. The disc is meant to
/// CLIP in LDR and to bloom under the HDR chain — it is the sun.
struct SunDisc {
    bool  enabled = false;
    /// Unit vector FROM the scene TOWARDS the sun, in world space.
    float dir[3] = { 0.0f, 1.0f, 0.0f };
    /// The disc's angular DIAMETER in degrees (the real sun is 0.53).
    float angularDiameterDeg = 0.53f;
    Colour colour { 1.0f, 1.0f, 1.0f, 1.0f };
    /// Do reflection-probe captures contain it? Off by default: the sun's
    /// energy already reaches glossy surfaces through the directional light's
    /// own specular highlight, so a captured disc is a second sun.
    bool  inProbes = false;

    bool operator==(const SunDisc &o) const {
        if (enabled != o.enabled) return false;
        if (!enabled) return true;   // a disabled disc has no other state
        return dir[0] == o.dir[0] && dir[1] == o.dir[1] && dir[2] == o.dir[2] &&
               angularDiameterDeg == o.angularDiameterDeg &&
               colour.r == o.colour.r && colour.g == o.colour.g &&
               colour.b == o.colour.b && inProbes == o.inProbes;
    }
    bool operator!=(const SunDisc &o) const { return !(*this == o); }
};

struct SkyDesc {
    /// NoSky removes the sky (the View's background shows through).
    SkyMode   mode = SkyMode::NoSky;
    /// SkyMode::Equirectangular: the lat-long image. Ignored otherwise.
    TextureId equirect = 0;
    /// SkyMode::Cubemap: six face textures, in the order +X, -X, +Y, -Y, +Z,
    /// -Z, each face seen from INSIDE the cube looking down that WORLD axis
    /// (the backend converts to whatever handedness its cubemaps use). They
    /// also feed environment reflections — a cubemap sky needs no
    /// `reflectionFaces`. Ignored in every other mode.
    TextureId faces[6] = { 0, 0, 0, 0, 0, 0 };

    /// SkyMode::Atmosphere: the analytic sky's parameters. Ignored otherwise.
    AtmosphereSky atmosphere;

    /// ENVIRONMENT REFLECTIONS (IBL), independently of the sky.
    ///
    /// `reflections == false` means "this description says nothing about
    /// reflections": whatever is bound stays bound. That is the state a
    /// cubemap sky is in (its own faces are the source), and it is also what a
    /// host says when it could not produce reflection faces this time and
    /// prefers the previous ones to no reflections at all.
    ///
    /// With `reflections == true`, `reflectionFaces` are six square, equally
    /// sized world-axis faces (same order as `faces`) which become the
    /// GGX-prefiltered cubemap every PBR material samples — this is how
    /// equirectangular and CPU-baked skies get what a cubemap sky gets for
    /// free; the host resamples its equirect image into six faces and pushes
    /// them here. The mip chain is a roughness PREFILTER, not a box mip chain,
    /// so a rough metal reads the hemisphere around its reflection vector
    /// instead of one blurred face. Six zeros CLEAR the reflections. The face
    /// textures are copied; the caller may destroy them afterwards.
    bool      reflections = false;
    TextureId reflectionFaces[6] = { 0, 0, 0, 0, 0, 0 };

    /// Exact equality, like every other change-guard on this boundary. Texture
    /// ids are monotonic per scene and never recycled, so equal ids really are
    /// the same pixels.
    /// THE SUN DISC, drawn over this sky (SunDisc above). A THIRD independent
    /// half: it changes every time the sun light is rotated, and re-uploading
    /// the sky or rebuilding the IBL cubemap for that would be absurd.
    SunDisc   sun;

    bool operator==(const SkyDesc &o) const {
        return sameSky(o) && sameReflections(o) && sun == o.sun;
    }
    bool operator!=(const SkyDesc &o) const { return !(*this == o); }

    /// The two halves, separately: the backend rebuilds the sky and the
    /// reflection cubemap independently (they were two verbs for exactly that
    /// reason), so a description that changes only its reflection faces must
    /// not tear the sky down and back up.
    bool sameSky(const SkyDesc &o) const {
        if (mode != o.mode) return false;
        if (mode == SkyMode::Equirectangular) return equirect == o.equirect;
        if (mode == SkyMode::Cubemap) {
            for (int i = 0; i < 6; ++i) if (faces[i] != o.faces[i]) return false;
        }
        if (mode == SkyMode::Atmosphere) return atmosphere == o.atmosphere;
        return true;
    }
    bool sameReflections(const SkyDesc &o) const {
        if (reflections != o.reflections) return false;
        if (!reflections) return true;
        for (int i = 0; i < 6; ++i)
            if (reflectionFaces[i] != o.reflectionFaces[i]) return false;
        return true;
    }
};

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
    /// A DISTORTION emitter (POST_LOOKS_SPEC §5.3 4b): the particles draw no
    /// colour at all — they are rendered into the view's distortion field and
    /// warp whatever is behind them (heat haze, shock rings, a jet's exhaust).
    /// `texture` is then read as the DISPLACEMENT map, exactly as a
    /// ShadingModel::Distortion material reads its normal-map slot (R/G
    /// remapped to [-1,1]), and each particle's colour ALPHA is its strength
    /// (the emitter's colour range and any colour ramp fade it over life).
    /// `additive` and `alphaHash` are ignored: the field is always alpha
    /// blended. A distortion emitter is invisible to every pass but the
    /// distortion pass, so with no distortion in the view's post chain the
    /// frame is byte-identical to the emitter not existing. It is a TOPOLOGY
    /// property: flipping it rebuilds the definition.
    bool      distortion = false;
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

/// A SCENE'S SHADOW REQUEST (ENGINEERING_DEBT_SPEC.md item 4, §9's reading of
/// it: the globalness is real, so the API HIDES it rather than pretending to
/// remove it).
///
/// The backend has ONE shadow filter and ONE shadow atlas for every scene it
/// draws — Engine::setShadowFilter and Engine::setShadowResolution carry the
/// detail and are still the low-level truth. What hosts actually have is a
/// per-scene answer they derived themselves (the softest filter and the
/// largest map any shadow-casting light in THIS scene asked for, or a World
/// panel override that pins both), and before this shape they pushed it
/// through the two global setters guarded by hand-written read-before-write
/// tests — three of them, in the mirror, one per knob per policy branch.
///
/// Scene::setShadowSettings takes the resolved per-scene answer and applies it
/// to the global state, dropping a push that asks for what is already in
/// force. The last scene to push a value owns it, exactly as before — the same
/// contract the fixed simulation clock documents (ENGINEERING_DEBT_SPEC A4.2): the host pushes the simulated seconds as the engine frame delta.
struct ShadowDesc {
    /// false = "this scene has no opinion about the filter": whatever is in
    /// force stays. That is a scene with no shadow-casting light and no pinned
    /// quality — it must not drag the filter back to a default and undo
    /// another scene's request.
    bool         hasFilter = false;
    ShadowFilter filter = ShadowFilter::Soft;
    /// 0 = "no opinion about the map size" (same reasoning as hasFilter).
    /// Otherwise a request in pixels; the backend clamps to [256, 8192].
    /// NOT cheap when it CHANGES (the shadow node and every workspace that
    /// references it are rebuilt) — which is why the backend compares first.
    unsigned     resolution = 0;

    bool operator==(const ShadowDesc &o) const {
        return hasFilter == o.hasFilter && (!hasFilter || filter == o.filter) &&
               resolution == o.resolution;
    }
    bool operator!=(const ShadowDesc &o) const { return !(*this == o); }
};

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
    /// Scene::setNodeLightMask carries the full contract, and
    /// Scene::setNodeCastShadow is the escape hatch for the caster side.
    unsigned  lightMask = 0xFFFFFFFFu;

    /// FORWARD SHADING PRIORITY — DIRECTIONAL LIGHTS ONLY. 0 is the sun.
    /// Informational at the backend: the host resolves which directional is
    /// the sun (one resolver, iris::Scene::sunLight) and says so in
    /// `primaryDirectional`; this number is carried so the renderer can report
    /// it back and so the two cannot silently disagree.
    int       forwardShadingPriority = 0;

    /// Is this the scene's SUN — the one directional light allowed to cast?
    /// Our shadow node has a single directional slot (three PSSM splits at slot
    /// 0), so a second casting directional would be picked by Ogre's own
    /// castShadows-then-light-id sort, i.e. by creation order, silently and
    /// differently across reloads. setLight forces `castShadows` false on a
    /// directional that is not the primary, which leaves Ogre's sort exactly
    /// one candidate. Meaningless on point/spot/area lights (always true).
    bool      primaryDirectional = true;

    /// "Is this the same light state I last pushed?" — the guard a host with a
    /// per-frame push loop needs (setLight is ~20 backend setters, including an
    /// attenuation solve that rewrites the light's local AABB). Exact
    /// comparison, like PbrParams::operator==.
    ///
    /// EVERY FIELD setLight READS IS HERE: add a field to this struct and this
    /// must grow with it, or the new field silently stops reaching the backend
    /// after the first push. (It lives beside the fields for that reason — the
    /// mirror carried a hand-written `sameLight()` for it, one file away from
    /// the struct it had to track.)
    bool operator==(const LightDesc &o) const {
        return type == o.type && colour == o.colour && intensity == o.intensity &&
               range == o.range && spotAngleDegrees == o.spotAngleDegrees &&
               spotSoftness == o.spotSoftness && spotFalloff == o.spotFalloff &&
               castShadows == o.castShadows &&
               rectWidth == o.rectWidth && rectHeight == o.rectHeight &&
               doubleSided == o.doubleSided && accurate == o.accurate &&
               iesProfilePath == o.iesProfilePath && texturePath == o.texturePath &&
               lightMask == o.lightMask &&
               forwardShadingPriority == o.forwardShadingPriority &&
               primaryDirectional == o.primaryDirectional;
    }
    bool operator!=(const LightDesc &o) const { return !(*this == o); }
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
/// KNOWN LEAK (audit; ENGINEERING_DEBT_SPEC.md item 4). This is an X11 concept
/// in a supposedly platform-neutral boundary, and the macOS host has arrived
/// since that note was written without settling it: macOS simply leaves it 0
/// and the Linux host fills it from QNativeInterface::QX11Application
/// (src/bridge/enginehost.cpp). Still not fixed HERE because the fix is not a
/// rename: window and display are one surface identity, so the honest shape is
/// a single native-surface value that both hosts fill and OgreView reads —
/// which is host-side work (a different lane's files) plus a View change, for
/// no behaviour difference. Recorded, deliberately not smuggled in.
using NativeDisplayHandle = unsigned long long;

/// Opaque handle to something in a Scene. 0 is "none". Ids are per-Scene and
/// monotonic: a removed node's id is NEVER reused, so a stale id is harmless.
using NodeId = unsigned int;

// ---- Global illumination (scene-level; SPECS/PHOTON_SPEC.md is the live
// ---- program doc — GI_SPEC.md and GI_UNIFIED_SPEC.md are its earlier specs,
// ---- written when the program was called Rayon) ----
/// Which GI system lights the scene. Off is the default everywhere — GI must
/// never cost anything unless the author turns it on.
/// INSTANT RADIOSITY IS GONE (PHOTON_SPEC §7 E2 (4), 2026-09-15). It was the
/// `Low` tier's technique: a CPU ray trace from ONE light, planting virtual
/// point lights — so it saw one light, no emissive surface and no area light,
/// re-traced on every light move, and could not feed the irradiance field
/// because it produced no volume to feed it from. Photon's Low tier is two
/// camera-centred voxel cascades at 64^3 with the field on instead, which is
/// cheaper on the frame, sees every light, and is the SAME arm as the tiers
/// above it rather than a second lighting model nobody could reason about.
///
/// The ORDINALS MOVED with it (Vct 2 -> 1, VctPccHybrid 3 -> 2). That is safe
/// and deliberate: every serializer in the tree writes these as STABLE STRINGS
/// and says so at the table (`scenewriter.cpp`, "the enum ints must stay free to
/// be reordered"), and a document that still says `instant_radiosity` reads back
/// as `Vct` — which is what its tier resolves to now.
enum class GiMode {
    Off,
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

/// WHAT THE SCENE WAS AUTHORED FOR, as far as hardware ray tracing goes
/// (Scene::setRayTracing; iris::RayTracingMode is the document's twin, and the
/// owner's decision is recorded there in full).
///
/// `Auto` traces wherever the machine can and falls back silently; `On` does
/// exactly the same in the renderer and differs only OUTSIDE it (the editor
/// tells the author when the machine falls short — nothing here can conjure
/// ray hardware); `Off` never traces, even on a device that advertises ray
/// queries, so a scene can look and cost the same on every machine.
///
/// The renderer asks ONE question of all this — `Scene::rayTracingResolved()`
/// — and every ray-consuming stage reads that predicate and nothing else.
enum class RayTracingMode { Auto, Off, On };

/// WHY THE REFLECTION-PROBE GRID WAS LAST MARKED STALE (ENGINE_CACHE_POLICY_SPEC
/// §2 P1/P7) — GiStatus::lastStaleReason.
///
/// The probes are a CACHE: captured once, reused every frame, and re-captured
/// only when one of their inputs changes. Every input that can change what a
/// probe would capture marks the grid stale with its reason, and the per-frame
/// update budget spends itself on stale probes only — so a still scene
/// re-captures nothing at all. This names the input, so "why is the sweep
/// running?" is a reading rather than a guess.
///
///   `Rebuild`   a from-scratch build placed a fresh grid (mode, quality, grid,
///               bounds, anything destroyed) — every probe owes a capture
///   `Refresh`   a GI re-solve (the mirror's settle after a drag, or
///               world.refreshGi()) — the voxels moved under the probes
///   `Moved`     geometry the probes capture moved, arrived, left, or was
///               shown, hidden or flagged helper — GI geometry through the
///               movement scan, UNLIT geometry through a probe-only stale that
///               never touches the voxels. A DECAL added, edited, moved or
///               removed is this reason too (it paints a surface the probes
///               capture, and it is never voxelized)
///   `Light`     a light was added, switched on or off, or changed a parameter
///   `Material`  a material parameter or texture used by visible geometry changed
///   `Sky`       the sky or its reflection cubemap changed
///   `Ambient`   the ambient (flat, hemisphere or sky SH) changed
///   `Fog`       the fog description changed
///   `Mobility`  an object's mobility class changed, so it left (or joined) the
///               channel the probes capture — the probes holding its photograph
///               owe one re-capture (see MobilityChange)
///   `Camera`    the CAMERA moved, and a camera-following cache had to redo work
///               (Photon's cascade chain re-centring — the one reason in this
///               list that is not an edit, which is exactly why it needs its own
///               name: a capture must be able to separate the cost of walking
///               around a scene from the cost of changing it)
///   `None`      nothing has staled the grid since the scene was created
///
/// TIME-VARYING CONTENT IS FROZEN in the probes (REALTIME_REFLECTIONS_SPEC O4,
/// lead decision A, 2026-09-12): a posing rig, a particle system, a clock-driven
/// material or a live/video texture is captured as it was when the grid last
/// re-captured, and stales nothing on its own. SSR and planar reflections show
/// such content live; probes are the static-environment layer.
enum class GiStaleReason { None, Rebuild, Refresh, Moved, Light, Material, Sky, Ambient, Fog, Mobility,
                           Camera };

/// Scene-level GI state, pushed idempotently via Scene::setGlobalIllumination.
struct GiParams {
    GiMode    mode    = GiMode::Off;
    GiQuality quality = GiQuality::Medium;
    /// THE LIT VOLUME IS THE RENDERER'S, AND THESE THREE ARE TEST LEVERS
    /// (owner decision D8, 2026-09-13; PHOTON_SPEC §10's E2 row).
    ///
    /// Nothing a user can reach writes them: the document carries no bounds at
    /// all, `world.gi` refuses `boundsMin`/`boundsMax`/`autoBoundsMax` BY NAME,
    /// the World panel's rows and its Fit Bounds button are deleted, and
    /// SceneMirror never touches them. They survive for one reason — roughly
    /// fifteen engine suites PIN a volume so that a pixel assertion is about the
    /// thing it names and not about where the automatic fit happened to land —
    /// and they carry `test` in their names so that reading this struct cannot
    /// suggest otherwise. They stay inside `operator==` on purpose: a test that
    /// moves the pinned volume must get a rebuild, like any other configuration.
    ///
    /// `testBoundsMin == testBoundsMax` (the default) is "no pin": the backend
    /// fits the volume to the scene's lit geometry, under `kAutoGiBoundsMax`.
    Vec3      testBoundsMin, testBoundsMax;
    /// The ceiling on an AUTOMATIC fit, in metres — a test lever over
    /// `kAutoGiBoundsMax` (OgreGi.cpp), which is the shipped 64 m and the only
    /// value anything outside a suite has ever used. Negative (the default) is
    /// "the engine's own"; 0 disables the ceiling; anything else replaces it.
    /// The rationale for 64 m — and for why the ceiling is in METRES rather
    /// than metres-per-voxel, which would shrink the lit world as the quality
    /// dial goes down — is at the constant.
    float     testAutoBoundsMax = -1.0f;
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
    /// PROBE CAPTURE SIZE — the pixel size of one cube face, and the single
    /// biggest VRAM lever the hybrid has (REFLECTION_PROBE_AUDIT §4.2: a probe
    /// costs `6 * size^2 * bytes * mips`, so halving it quarters the grid).
    /// 0 = follow the quality dial (128 at Low, 256 at Medium and High since
    /// the 2026-09-13 halving: 512 spent 16 MiB per probe — 512 MiB on a
    /// 32-probe room — for detail the roughness mip chain blurs away). Any
    /// other value is taken verbatim, clamped to 64..1024 and rounded down to
    /// a power of two, because Ogre's IBL mip chain is built from it.
    int       probeCaptureSize = 0;
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
    /// A RATE, not a subset, and spent only on STALE probes (ENGINE_CACHE_POLICY_
    /// SPEC P1, 2026-09-12): the probes are a cache. Every input that changes
    /// what a probe would capture — a GI rebuild or re-solve, STILL geometry
    /// moving, arriving, leaving or being shown/hidden, a light, material, sky,
    /// ambient or fog change, an object's mobility changing — marks the grid
    /// stale (GiStatus::lastStaleReason names it; time-varying content is
    /// frozen, and MOVABLE objects are not in a capture at all, so moving one
    /// is not an input — see GiStaleReason and MobilityStatus), and
    /// each frame the engine re-captures up to `updateBudget` of the
    /// highest-priority stale probes. So every probe re-captures within
    /// ceil(probeCount / updateBudget) frames OF A CHANGE, whatever the priority
    /// does — and a still scene re-captures NOTHING (it used to re-capture one
    /// probe every frame for ever). Priority (staleness x proximity to the
    /// tracked camera x covers-something-that-just-moved) only decides the ORDER
    /// of the catch-up, which is what puts the probes the viewer can see, and
    /// the ones the moving object is inside, at the front of it.
    ///
    /// 0 = PAUSED: no probe re-captures, and the mirror stops auto-refreshing GI
    /// as well (it is the same "GI is frozen" intent). That is the pre-fix-wave
    /// shipped behaviour, kept as one switch.
    ///
    /// 1 (the default) is a realtime editor: at most one probe face-set per
    /// frame while anything is stale, measured at ~2.1 ms in a Debug build at
    /// Medium quality (256px faces), and nothing at rest. Raising it buys
    /// catch-up latency at a linear cost. `GiStatus::probeUpdatesPerFrame`
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
    /// the pass-buffer under-report, fixed by ogre-patch 0050) — it lands within about 15% of the brightness it takes
    /// over from, which is what makes `ddgiIntensity` a trim rather than a
    /// correction.
    ///
    /// GiToggle::Auto means "let the quality tier decide", and the deciding
    /// happens DOCUMENT-SIDE: the Photon tier (GI_UNIFIED P2) writes a concrete
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
    /// the pass-buffer under-report (fixed by ogre-patch 0050), which was collapsing every irradiance
    /// lookup onto one texel; it is corrected here and the number does not
    /// survive it. GI_UNIFIED_SPEC addendum item 2 should be read with that in
    /// mind.) The knob stays because the two terms are different integrals and
    /// a scene may want the trim; clamped to [0, 64], and 0 is a legitimate
    /// "field bound, contributing nothing" for A/B measurement.
    float     ddgiIntensity = 1.0f;
    /// THE AMBIENT SKY-VISIBILITY STRENGTH — the Photon ambient fix's one dial
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

    // ---- PHOTON: camera-centred voxel cascades (PHOTON_SPEC P0) -------------

    /// ONE camera-centred cascade of the Photon cascade chain.
    ///
    /// `halfSize` is the half-extent in METRES of the cube this cascade covers
    /// (the sample set's inner cascade is 5 m, i.e. a 10 m box); `resolution`
    /// its voxel resolution per axis; `stepCells` how many CELLS the camera may
    /// travel before the cascade re-centres (the pin's `cameraStepSize`, so
    /// the scroll distance is `stepCells * 2 * halfSize / resolution`).
    ///
    /// The step is in cells and not in metres on purpose: it is what keeps a
    /// re-centred cascade aligned to the same world lattice it was built on,
    /// which is the property that makes a scroll a whole number of cells and
    /// stops the bounce sliding under the geometry.
    struct GiCascadeDesc {
        float halfSize   = 0.0f;
        int   resolution = 0;
        float stepCells  = 0.0f;
        bool operator==(const GiCascadeDesc &o) const {
            return halfSize == o.halfSize && resolution == o.resolution &&
                   stepCells == o.stepCells;
        }
    };
    /// THE PHOTON SWITCH. False (the default) is the single scene-fitted voxel
    /// volume this engine has always built: one box around the content, and
    /// nothing outside it bounces. True builds N camera-centred cascades
    /// instead, chained through `VctLighting::addCascade`, so the bounce
    /// follows the camera and what escapes the outermost cascade reads the
    /// ambient (the Sky Light) rather than a wall of darkness.
    ///
    /// Only meaningful in `Vct` and `VctPccHybrid`. It changes NOTHING about
    /// the picture when off — the arm it selects is chosen in `rebuildVct`.
    bool      cascades = false;
    /// How many cascades to build, 1..8. 0 means "the table below decides", and
    /// when the table is empty too, the engine's own tier table does.
    int       cascadeCount = 0;
    /// The cascade table. Entries [0, cascadeCount) are used; a zero
    /// `resolution` or `halfSize` in a used entry falls back to the tier table.
    /// The engine's default is the Ogre sample's set (5 m@128, 10 m@128,
    /// 15 m@64, 60 m@64), which PHOTON_SPEC §5 measured the cadence of.
    GiCascadeDesc cascadeSet[8];
    /// THE PER-CASCADE INSTANCE BUDGET (PHOTON_SPEC §7 E2 (1), audit B7).
    ///
    /// The raster voxeliser's price is the GEOMETRY INSIDE THE REGION and
    /// nothing else — measured at ~13 us per enclosed instance (P0 §6.2), which
    /// is 3-6 ms for a room and 60-110 ms for a dense world, on the frame
    /// thread, for ONE cascade. A budget is the patch-free lever: each cascade
    /// voxelises AT MOST this many objects, and it keeps the ones that matter
    /// most to it — ranked by how much of one of ITS OWN voxels each object
    /// fills (world size / cell), largest first, ties broken by distance to the
    /// cascade's centre.
    ///
    /// Ranking by size-in-cells and not by size is what makes one number right
    /// for a whole chain: the same 1 m crate is 13 cells across for the inner
    /// cascade and half a cell for the outer one, so the outer cascade spends
    /// its budget on the buildings and the inner one on the crates.
    ///
    /// 0 (the default) is NO BUDGET, and it is the shipped arm exactly: the
    /// attach set is the whole size-filtered scene, attached once, and Ogre's
    /// own region cull decides what each build voxelises (the attach-once rule
    /// in OgreGi.cpp, which exists because re-selecting drops the voxeliser's
    /// mesh bookkeeping and re-uploads every buffer). A budget necessarily
    /// gives that up for the cascades it binds, because WHICH objects are
    /// nearest changes as the cascade scrolls — so it re-selects only when the
    /// chosen set actually differs from the one attached.
    ///
    /// `GiStatus::cascades[].items` reports what each cascade voxelised and
    /// `[].attached` what it holds, so a budget that is biting is a reading.
    int       cascadeInstanceCap = 0;
    /// THE FAR-FIELD PROXY: a cascade voxelises the BAKED LOD LEVEL that fits
    /// its own cell (ATOM stage 1's hand-off, SPECS/NANITE_SPEC.md §7 — the
    /// rule is `lodLevelForWorldError` and the site is
    /// OgreScene::cascadeVoxelLod). True (the default) spends the chain; false
    /// voxelises every cascade at the authored level, which is what the arm did
    /// before ogre-patch 0064 existed.
    ///
    /// It is here — an engine parameter rather than a document row — because it
    /// is the A/B: the same scene, the same chain, one term moved, so the cost
    /// it saves and the voxels it moves can be measured against each other in
    /// ONE process (gi.cascade_lod does exactly that). A mesh with no LOD chain
    /// is unaffected either way, and that is most of what a scene holds today:
    /// only IMPORTED static meshes are baked with one (document primitives are
    /// not).
    bool      cascadeVoxelLod = true;

    /// "Is this the same GI configuration I last pushed?" Exact, like every
    /// other change guard here — and load-bearing rather than cosmetic: a GI
    /// push is a teardown plus a re-voxelize plus (in the hybrid) every probe
    /// re-rendered twice, so a comparison that missed a field would either
    /// rebuild GI every frame or never notice a dial moving.
    ///
    /// EVERY FIELD setGlobalIllumination READS IS HERE — add one above and add
    /// it here. (The mirror hand-wrote this comparison over 24 fields; keeping
    /// it beside the struct is what makes "add a field" a one-place edit.)
    ///
    /// THE THREE TUNING FLOATS ARE DELIBERATELY ABSENT (PHOTON_SPEC §7 E2 (8),
    /// audit A F6): `ddgiIntensity`, `ddgiAmbient` and `rayMarchStepScale` are
    /// read per frame, so they take effect through `Scene::setGiTuning` without
    /// a rebuild — and while they were IN this comparison every tick of those
    /// three sliders was a from-scratch teardown and re-voxelisation (N of them
    /// under a cascade chain). `giTuningEqual` is their comparison; a host
    /// pushes on `!(a == b)` for the configuration and on `!a.giTuningEqual(b)`
    /// for the tuning.
    bool operator==(const GiParams &o) const {
        return mode == o.mode && quality == o.quality &&
               numBounces == o.numBounces && testAutoBoundsMax == o.testAutoBoundsMax &&
               pccProbesX == o.pccProbesX && pccProbesY == o.pccProbesY &&
               pccProbesZ == o.pccProbesZ &&
               probeHdr == o.probeHdr && probeShadows == o.probeShadows &&
               probeCaptureSize == o.probeCaptureSize &&
               probeOverlap == o.probeOverlap &&
               probeSnapDeviation == o.probeSnapDeviation &&
               probeSnapSidesMin == o.probeSnapSidesMin &&
               probeSnapSidesMax == o.probeSnapSidesMax &&
               updateBudget == o.updateBudget &&
               cascadeVoxelLod == o.cascadeVoxelLod &&
               ddgi == o.ddgi &&
               testBoundsMin == o.testBoundsMin && testBoundsMax == o.testBoundsMax &&
               cascades == o.cascades && cascadeCount == o.cascadeCount &&
               cascadeInstanceCap == o.cascadeInstanceCap &&
               cascadeSetEqual(o);
    }
    /// The three values `Scene::setGiTuning` pushes, compared on their own.
    bool giTuningEqual(const GiParams &o) const {
        return ddgiIntensity == o.ddgiIntensity && ddgiAmbient == o.ddgiAmbient &&
               rayMarchStepScale == o.rayMarchStepScale;
    }
    /// The cascade table, compared only over the entries in USE — a table
    /// beyond `cascadeCount` is not part of the configuration.
    bool cascadeSetEqual(const GiParams &o) const {
        for (int i = 0; i < cascadeCount && i < 8; ++i)
            if (!(cascadeSet[i] == o.cascadeSet[i])) return false;
        return true;
    }
    bool operator!=(const GiParams &o) const { return !(*this == o); }
};

/// WHAT A QUALITY TIER PHYSICALLY IS, IN ONE PLACE (render audit A5, lane
/// CRUD-RENDER-1).
///
/// Every number a Photon quality tier decides lives here and NOWHERE else: the
/// engine reads it (`OgreScene::giVoxelResolution`, `resolveCascadeTable`'s
/// fallback, `buildPcc`'s probe resolution and its HDR/shadow defaults) and so
/// does the HOST, which is the point — the app's five tier tooltips used to
/// describe a renderer that did not exist ("Medium voxelizes at twice the
/// resolution" when both are 64; "32/64/128 voxels per axis" when the cascade
/// chain, which is on at every tier, uses 64/64/128; "128^3 at High/Epic" when
/// two of the four cascades are 64). They are GENERATED from this table now,
/// through `world.tierTable()`, so a tier's description cannot drift from what
/// the tier does.
///
/// WHICH COLUMN OF THE TIER TABLE A VIEW READS (lane V1-RIG item 4,
/// PHOTON_SPEC D2 "VR = world-space caches only ... a VR cascade set with
/// larger outer steps").
///
/// A headset renders the chain FIVE TIMES over: 2160x2376 per eye against a
/// desktop 1080p is 10.26 against 2.07 megapixels, and the budget HALVES (11.1
/// ms at 90 Hz against 16.7 at 60). The cascade chain's price has two halves
/// and only one of them is per pixel:
///
///   THE PIXEL MARCH — measured at Quest Pro size on the rig, one process, one
///   pose, clocks locked (spikes/v1-rig): the opaque pass reads 3.815 ms at
///   four 64^3 cascades, 3.253 at three and 2.568 at two, i.e. **0.31 ms PER EYE
///   PER CASCADE**. It is the term a VR profile can actually buy back.
///
///   THE REBUILD BURSTS — 2.0-2.6 ms of GPU per cascade rebuild, one per frame
///   at most, and since the field's follow moved off the rebuild frame no walk
///   frame crosses 11.1 ms at either tier. So the steps are no longer the
///   binding constraint; halving the OUTERMOST cascade's rate is headroom, not
///   a rescue.
///
/// So the VR column is the tier's own chain with the REDUNDANT MIDDLE cascade
/// dropped and the OUTERMOST STEP DOUBLED. The two halves apply independently:
/// four rows become three at Medium, High and Epic, while LOW KEEPS ITS TWO (it
/// has no middle to drop) — but Low's outer step doubles with everybody else's,
/// so no tier's VR column equals its desktop one. Nothing about a "room", a
/// volume or an axis count enters it: the reach is unchanged, the inner cell is
/// unchanged, and what is given up is one hand-over in the mid field (Medium's
/// 10 m row sits between a 5 m and a 15 m one) and up to twice the off-centring
/// of the outermost cascade — 30 m on Medium's 120 m box at 1.875 m per cell
/// instead of 15, 60 m on Low's 40 m box at 0.625 m per cell instead of 30.
enum class GiViewProfile {
    Desktop = 0,
    Vr      = 1,
};

/// It is a pure function of the quality dial and the view profile: no scene, no
/// device, no Ogre.
struct GiQualityFacts {
    /// The engine's cascade chain for this tier, innermost first, as
    /// `resolveCascadeTable()` builds it when nothing is pinned. `stepCells` is
    /// left at 0 — the step is DERIVED from the chain (resolveCascadeTable does
    /// it), not a property of the tier.
    GiParams::GiCascadeDesc cascades[4];
    /// How many entries of `cascades` are in use.
    int   cascadeCount = 0;
    /// The SINGLE scene-fitted volume's resolution per axis — the arm used when
    /// the chain is off (`GiParams::cascades == false`).
    unsigned voxelResolution = 64u;
    /// One reflection-probe cube face, in pixels, when the scene pins no size.
    unsigned probeFaceSize = 256u;
    /// What `GiToggle::Auto` resolves to for the two expensive probe options.
    bool  probeHdrDefault = false;
    bool  probeShadowsDefault = false;
};

/// THE TIER TABLE. Hand-edit this and every reader — engine and app — moves
/// with it. `profile` picks the column (see GiViewProfile for the measurement
/// behind the VR one).
inline GiQualityFacts giQualityFacts(GiQuality quality,
                                     GiViewProfile profile = GiViewProfile::Desktop)
{
    GiQualityFacts f;
    switch (quality) {
    case GiQuality::Low:
        // LOW IS 64^3 IN THE CHAIN, NOT THE QUALITY DIAL'S 32 (PHOTON_SPEC §7
        // E2 (4)): the number that decides whether its bounce means anything is
        // the CELL, and at 32 the inner cascade's cell is 0.31 m, which smears
        // a room's own walls. TWO cascades, because reach is what stops a
        // corridor going black and the far one is the cheap one.
        f.cascades[0] = {  5.0f, 64, 0.0f };
        f.cascades[1] = { 20.0f, 64, 0.0f };
        f.cascadeCount = 2;
        f.voxelResolution = 32u;
        f.probeFaceSize   = 128u;
        break;
    case GiQuality::High:
        f.cascades[0] = {  5.0f, 128, 0.0f };
        f.cascades[1] = { 10.0f, 128, 0.0f };
        f.cascades[2] = { 15.0f,  64, 0.0f };
        f.cascades[3] = { 60.0f,  64, 0.0f };
        f.cascadeCount = 4;
        f.voxelResolution = 128u;
        f.probeFaceSize   = 512u;
        // The two expensive probe options are ON at this tier and only here
        // (REFLECTIONS_ADOPTION_SPEC P3a/P3b) — the pair `GiToggle::Auto` reads.
        f.probeHdrDefault     = true;
        f.probeShadowsDefault = true;
        break;
    default:   // Medium: the same reach as High, at its own resolution
        f.cascades[0] = {  5.0f, 64, 0.0f };
        f.cascades[1] = { 10.0f, 64, 0.0f };
        f.cascades[2] = { 15.0f, 64, 0.0f };
        f.cascades[3] = { 60.0f, 64, 0.0f };
        f.cascadeCount = 4;
        f.voxelResolution = 64u;
        f.probeFaceSize   = 256u;
        break;
    }
    // ---- THE VR COLUMN (GiViewProfile, above) ------------------------------
    // ONE transform over the desktop rows, so the two columns cannot drift: the
    // middle cascade goes and the outermost steps twice as far. `stepCells` on
    // a row means "pinned"; the engine derives the rest.
    if (profile == GiViewProfile::Vr) {
        // Both halves are independent, and LOW GETS ONLY THE SECOND: with two
        // rows there is no middle to drop (corrected 2026-09-18 — the first
        // note claimed Low was untouched, which the doubled outer step makes
        // false).
        if (f.cascadeCount >= 4) {
            // Drop index 1 — the row closest in reach to the one outside it
            // (Medium 5/10/15/60, High 5/10/15/60 at its own resolutions), so
            // the near field and the far reach are both untouched.
            for (int i = 1; i + 1 < f.cascadeCount; ++i) f.cascades[i] = f.cascades[i + 1];
            --f.cascadeCount;
        }
        if (f.cascadeCount > 0) {
            // The engine's own outer default is 8 cells (OgreGi.cpp
            // kOuterStepCells); doubling it halves the most expensive rebuild
            // in the chain. Pinned here rather than in the engine so the table
            // is the one place a tier's physics lives.
            f.cascades[f.cascadeCount - 1].stepCells = 16.0f;
        }
    }
    return f;
}

/// One cascade's CELL in metres: the number that says what it can resolve.
inline float giCascadeCell(const GiParams::GiCascadeDesc &c)
{
    return c.resolution > 0 ? c.halfSize * 2.0f / float(c.resolution) : 0.0f;
}

// ---- THE NEAR-FIELD GUARANTEE (CASCADE-STEP-1, owner 2026-09-18) -----------
//
// THE RULE, stated before the arithmetic: THE INNERMOST CASCADE GUARANTEES A
// NEAR-FIELD RADIUS. Within `kGiNearFieldRadiusFraction` of its own half-size
// around the head, the near field is always VOXELISED BY THAT CASCADE — its box
// covers that radius at every moment of any walk, so the bounce a walker stands
// in is built at its cell size and never at the coarser cascade's behind it.
//
// It is NOT a claim about every cone sample. A cone aimed outward leaves the
// box and hands over to the coarser cascade by construction, carrying its age
// across the hop (SEAM-1, ogre-patch 0066), which is what a cone aimed outward
// should do. What the rule removes is the NEAR FIELD ITSELF changing resolution
// as the walker moves — the hand-over arriving at the wearer's feet.
//
// WHY IT NEEDS A RULE, AND WHAT THE BOUND HONESTLY IS.
//
// The re-centre test runs on an ABSOLUTE lattice: the planes sit every
// step*(1-h) metres of world space (h = kStepHysteresis) and the camera must be
// h*step past the plane it left, so a re-centre comes anywhere between h*step
// and step of travel from the camera the cascade was built for. `step` is the
// SUPREMUM of that travel, never the distance between two rebuilds. Three more
// terms sit on top of it:
//
//   * the re-centre QUANTISES the new centre onto the cascade's own cell
//     lattice, which leaves it up to one cell below the head on each axis;
//   * the test is a per-FRAME test — it fires on the first frame at which the
//     camera has already crossed, so the head is up to one frame of travel
//     (v*dt) past the threshold when the rebuild happens;
//   * and a rebuild can be DEFERRED: the frame spends at most one cascade, and
//     a field follow can own that slot, so each deferred frame is another v*dt.
//
//     off  =  step + cell + v*dt*(1 + deferrals)                 (per axis)
//     r    =  halfSize - off                                     (the inscribed radius)
//
// The engine cannot know v, so the motion terms are bought with ONE CELL OF
// SLACK. The STEP is chosen from
//
//     halfSize - step - 2*cell  >=  kGiNearFieldRadiusFraction * halfSize
//
// while `giCascadeGuaranteedRadius` reports the honest, motion-free
// `halfSize - step - cell`: the reported radius therefore clears the required
// one by about a cell, and that cell is the motion budget.
//
// WHAT THE SLACK BUYS, IN FRAMES — the number that makes this a walking-pace
// guarantee and says so. At 1.4 m/s on the 60 Hz clock a frame is 0.0233 m:
// Medium's margin (2.500 - 2.250 = 0.250 m) is ten frames and High's
// (2.344 - 2.250 = 0.094 m) is four, which covers the frame the test costs plus
// a follow or two owning the slot. At the editor's FLY speed of 15 m/s a frame
// is 0.25 m: Medium spends its whole margin on a single frame and High is
// 0.16 m inside the stated radius while the camera is still moving. THE
// GUARANTEE IS FOR WALKING PACE, deliberately — a flight is a camera in
// transit, and the only thing a cascade hand-over has to be invisible under is
// a person moving at a person's speed. (Both numbers are measured in
// spikes/cascade-step-1/MEASUREMENTS.txt; gi.cascades case 16b walks at 1.4 m/s
// from a NON-lattice start so the coupled worst phase is in the measurement.)
//
// The hysteresis costs no radius at all: kStepHysteresis shrinks the test
// lattice by the band and adds the band back on both sides (OgreGi.cpp), so the
// supremum stays exactly `step` and not `(1 + h) * step` — measured, 2.637 m of
// worst offset against a 2.656 m step before the slack landed.
//
// WHAT IT WAS BEFORE. The derived step was the pin's "every cascade steps the
// same distance", floored at half the resolution — and that floor BOUND on the
// innermost cascade at every tier but Low: 32 cells of a 64^3 5 m cascade is a
// 5 m step, i.e. step == halfSize, i.e. r = -cell. NOTHING was guaranteed: a
// wearer walking a straight line reached cascade 0's own face before it
// re-centred, and the metre in front of their eyes was read from cascade 1 —
// 3x coarser in the VR column (0.156 m -> 0.469 m). That is the artifact
// CASCADE-STEP-1 removes; the owner's acceptance is "we should never notice a
// change when walking around a scene".
//
// THE FRACTION is 0.45 of the half-size: 2.25 m of the 5 m inner cascade every
// tier ships, about one and a half paces ahead of the walker and more than the
// 2 m a room's near wall usually stands at. It is bought with rebuild
// frequency, linearly — the step falls from halfSize to a little under half of
// it, so cascade 0 re-voxelises about twice as often per metre walked, which is
// +2 ms of GPU per metre and no frame's peak worth naming (MEASUREMENTS.txt).
//
// THE TABLE THE RULE PRODUCES (r = guaranteed / required, metres; every row of
// every tier and both columns clears it, which gi.cascades case 16a asserts):
//
//   low    desktop  c0  2.344 m  r 2.500/2.250 | c1  5.000  r 14.375/9.000
//   low    vr       c0  2.344    r 2.500/2.250 | c1 10.000  r  9.375/9.000 (pinned)
//   medium desktop  c0  2.344    r 2.500/2.250 | c1  4.688  r  5.000/4.500
//                   c2  7.031    r 7.500/6.750 | c3 15.000  r 43.125/27.000
//   medium vr       c0  2.344    r 2.500/2.250 | c1  7.031  r  7.500/6.750
//                   c2 30.000    r 28.125/27.000 (pinned)
//   high   desktop  c0  2.578    r 2.344/2.250 | c1  5.156  r  4.688/4.500
//                   c2  7.031    r 7.500/6.750 | c3 15.000  r 43.125/27.000
//   high   vr       c0  2.578    r 2.344/2.250 | c1  7.031  r  7.500/6.750
//                   c2 30.000    r 28.125/27.000 (pinned)
//   (epic is high's table.)
//
// In cells, against what the derivation gave before this rule: Low c0 16 -> 15;
// Medium c0 32 -> 15, c1 24 -> 15, c2 16 -> 15; High c0 64 -> 33, c1 48 -> 33,
// c2 16 -> 15; every outermost cascade and every pinned step unchanged.
inline constexpr float kGiNearFieldRadiusFraction = 0.45f;

/// The near-field radius this cascade is REQUIRED to guarantee, in metres.
inline float giNearFieldRadius(const GiParams::GiCascadeDesc &c)
{
    return kGiNearFieldRadiusFraction * c.halfSize;
}

/// The near-field radius this cascade DOES guarantee at the step it carries:
/// `halfSize - step - cell`, in metres — the STILL-CAMERA bound, with the
/// re-centre's supremum travel and the centre's cell quantisation in it and the
/// motion terms (v*dt per frame the test costs and per frame a rebuild waits)
/// deliberately NOT in it, because the engine cannot know v. The step is chosen
/// with a cell of slack against the required radius (giNearFieldMaxStepCells)
/// and that cell is the motion budget: at 1.4 m/s it is four to ten frames.
/// Negative means the cascade guarantees nothing at all — the head can be
/// outside the box before it re-centres. Meaningless on a row whose step is
/// still 0 (not yet resolved).
inline float giCascadeGuaranteedRadius(const GiParams::GiCascadeDesc &c)
{
    const float cell = giCascadeCell(c);
    return c.halfSize - c.stepCells * cell - cell;
}

/// The most cells a cascade may step and still honour the rule, floored at one
/// cell (a step below one cell re-centres the volume for a fraction of a voxel)
/// and ceiled by the pin's own guard at half the resolution.
///
/// TWO cells are held back, not one: the first is the re-centre's own
/// quantisation (it is in the guaranteed radius) and the second is the MOTION
/// SLACK — the frame the per-frame test costs and the frames a deferred rebuild
/// waits, which the engine cannot price because it does not know the camera's
/// speed. One cell is 0.156 m at Medium and 0.078 m at High: ten and four
/// frames of walking, against a bare margin of 0.016 m at High without it,
/// which is less than ONE frame at 1.4 m/s.
inline float giNearFieldMaxStepCells(const GiParams::GiCascadeDesc &c)
{
    const float cell = giCascadeCell(c);
    if (cell <= 0.0f) return 1.0f;
    const float metres = c.halfSize - giNearFieldRadius(c) - 2.0f * cell;
    return std::max(1.0f, std::min(std::floor(metres / cell), float(c.resolution) * 0.5f));
}

/// RESOLVE THE DERIVED STEPS OF A WHOLE CHAIN, innermost first — the one place
/// the step of a cascade nobody pinned is decided, so the renderer's chain and
/// the chain a tooltip (world.tierTable) promises cannot drift.
///
/// A row that already carries a step (`stepCells > 0`) is PINNED and is left
/// exactly as it is: a pinned step is the author's own statement and is held
/// only by the pin's 1..resolution/2 guard, which the caller applies. Our own
/// tier tables pin only the outermost cascade (the VR column's 16 cells) and
/// they are checked against the rule by gi.cascades case 16a, not clamped
/// here.
///
/// Every other row gets the pin's `autoCalculateStepSizes(4)` shape
/// (OgreVctCascadedVoxelizer.cpp:131-161) written out here so it is ours to
/// tune (A7) — every finer cascade steps the same DISTANCE as the outermost
/// one, ceiled to whole cells and floored at half its resolution (the pin's own
/// guard against a step that outruns the volume) — MET WITH the near-field rule
/// above, which is a ceiling on it.
///
/// THE OUTERMOST CASCADE STEPS TWICE AS FAR AS THE REST (PHOTON_SPEC §7 E2 (1),
/// "the outer stepCells raised"), and the reason is a measurement, not symmetry.
/// The outermost cascade is the one that encloses the most geometry and resolves
/// the least, so it is BY FAR the most expensive rebuild in the chain — on the
/// 8,026-instance lattice it is 88.9 ms of GPU against cascade 0's 18.1, and
/// even on the Showroom at Epic it is the row that peaks
/// (spikes/photon-e2/BASELINE.md). Halving how often it runs halves that cost,
/// and what it buys with the frames it skips is that its 60 m box sits up to
/// 15 m off-centre instead of 7.5 — on a volume 120 m across, at 1.875 m per
/// cell, which is a quarter of a cell of parallax on the far bounce. It still
/// clears the near-field rule by a wide margin (43.1 m guaranteed against the
/// 27.0 asked of it), which is why the ceiling never bites there.
inline void giResolveCascadeSteps(GiParams::GiCascadeDesc *rows, int count)
{
    if (!rows || count <= 0) return;
    static const float kOuterStepCells = 8.0f;   // the pin's own value is 4
    static const float kInnerStepCells = 4.0f;
    const float cellLast = giCascadeCell(rows[count - 1]);
    for (int i = 0; i < count; ++i) {
        if (rows[i].stepCells > 0.0f) continue;          // pinned: not ours to decide
        const float cell = giCascadeCell(rows[i]);
        if (cell <= 0.0f) continue;
        float steps = (i + 1 == count) ? kOuterStepCells
                                       : std::ceil(kInnerStepCells * cellLast / cell);
        steps = std::max(1.0f, std::min(steps, float(rows[i].resolution) * 0.5f));
        // THE NEAR-FIELD RULE IS A CEILING ON ALL OF IT.
        rows[i].stepCells = std::min(steps, giNearFieldMaxStepCells(rows[i]));
    }
}

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
    /// METRES PER VOXEL of the volume `boundsMin/Max` describe — and UNDER A
    /// CASCADE CHAIN THAT IS THE OUTERMOST, COARSEST CASCADE'S CELL, because
    /// that is the volume those corners describe (render audit I-10).
    ///
    /// There is no single voxel size under a chain, so this scalar is the
    /// COARSEST one; `cascades[i].cell` is every one of them, and the innermost
    /// is what the eye is usually looking at. In the single-volume arm it is
    /// the largest axis divided by the tier's resolution
    /// (LIGHTING_PIPELINE_AUDIT L4.4), and then it is the whole answer: the
    /// shipped default project used to report 8.1 (a 1040 m volume at 128^3)
    /// and reports 0.5 with the automatic ceiling in force. 0 when there is no
    /// volume. It is the number that says whether GI in a scene means anything
    /// at all — a kilometre-wide volume at 128^3 is computing a constant.
    float  voxelMetres = 0.0f;
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
    /// The RESOLVED probe capture size in pixels per cube face — what
    /// `GiParams::probeCaptureSize` (0 = follow the quality dial) actually
    /// became. 0 when no grid was built.
    int    probeCaptureSize = 0;

    // ---- WHAT THE PROBES SAW: why there is (or is not) a grid ---------------
    // Owner decision 2026-09-13: "a user starts in the editor in a new project
    // with an open scene ... I would think the sky is your first reflection
    // asset." A probe that can see nothing but distance photographs the sky,
    // and the sky IBL is both cheaper and sharper than 18-32 captures of it. So
    // each candidate probe is kept or dropped by WHAT IT SAW — its own captured
    // depth, measured during the placement — and a scene whose probes all saw
    // nothing has no grid at all, which this says out loud instead of leaving
    // `probeCount 0` looking like a failure.
    //
    // (The rule that used to live here measured the scene for an ENCLOSURE —
    // facing walls, an axis count — and reported `probeEnclosedAxes` /
    // `probeGridRefused`. Both are deleted with it: owner+lead joint decision
    // 2026-09-14, "no room in any definition".)

    /// How many candidate probes the renderer built, photographed and then
    /// DROPPED because the VOLUME of the box their six faces measured was not
    /// smaller than the volume the renderer lit — i.e. what they could see was
    /// no nearer than the world itself. 0 in every mode but the hybrid.
    /// `probeCount 0` with this NON-ZERO is the open-scene answer (and the sky
    /// IBL is bound instead); `probeCount 0` with this ZERO while the mode is
    /// the hybrid is the silent-degradation failure gi.pcc_mirror exists to
    /// catch.
    int    probesDropped = 0;
    /// How many probes the renderer re-captures per frame — the RESOLVED
    /// `GiParams::updateBudget`, clamped to the probes that actually exist, and
    /// 0 whenever the probe arm did not build (FIX WAVE B1/B2). 0 in every mode
    /// but the hybrid. It is the CEILING a frame may spend: a stale grid
    /// catches up within ceil(probeCount / this) frames of a change, and a
    /// still scene spends nothing (probeCapturesLastFrame reads what was
    /// actually spent); see GiParams::updateBudget.
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
    /// How many material edits on this scene have CROSSED the reflection-probe
    /// gate — the point at which a material stops (or starts) being able to
    /// reflect anything, and its shader has to be rebuilt with or without the
    /// per-pixel probe loop (ogre-patch 0028). An ordinary parameter push is
    /// free; a crossing is a `flushRenderables` over every renderable wearing
    /// that datablock. The only workflow that crosses repeatedly is a user
    /// dragging Specular Color down through black and back, which crosses
    /// exactly twice; a monotonically rising count on a still scene is a
    /// defect. Cumulative, never reset.
    unsigned probeGateCrossings = 0;
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
    /// binds (the build converges it in one dispatch) and on the frame it is
    /// re-placed under a cascade chain (a follow converges it whole); it reads
    /// false while a progressive re-converge is in flight — after
    /// `refreshGiLighting`, or after cascade 0 re-voxelised at the same place.
    bool   ifdConverged = false;
    /// Probes the field is re-integrating per frame while a re-converge is in
    /// flight — the resolved figure, derived from `GiParams::updateBudget` and
    /// then clamped to the engine's dispatch rule (see OgreGi.cpp
    /// ifdProbesPerFrame: a dispatch of fewer rays than one thread group is an
    /// UNCAUGHT THROW in a release-built engine, so the clamp is mandatory).
    /// 0 when the budget is 0 (paused: nothing re-converges) or when there is
    /// no field.
    int    ifdProbesPerFrame = 0;
    /// WHERE THE FIELD IS: the corners of the volume its probe grid spans, as
    /// the engine placed it (the field enlarges that volume by one probe block
    /// per side for its own borders; this is the volume it was given). Equal
    /// corners = no field.
    ///
    /// In the single-volume arm this is the lit volume — the scene's fitted box
    /// — and it never moves without a rebuild. Under a Photon cascade chain it
    /// is CASCADE 0's box and it follows that cascade as the camera walks
    /// (PHOTON_SPEC E1), which is the only way to see, from outside, where the
    /// leak-free diffuse actually is.
    Vec3 ifdMin;
    Vec3 ifdMax;
    /// How many times the field has been re-placed onto cascade 0 since the
    /// last build: 0 in the single-volume arm and on a still camera, one per
    /// cascade-0 step while walking. Reset by a build, never by a scroll.
    unsigned long long ifdFollows = 0;

    // ---- THE PROBE CACHE (ENGINE_CACHE_POLICY_SPEC §2 P1/P6/P7) -------------
    // Reflection probes are re-captured only while STALE. These say what the
    // cache is doing, in counters rather than milliseconds: a still scene reads
    // probeCapturesLastFrame 0 and staleProbes 0, every frame.

    /// How many probes actually re-captured on the LAST rendered frame, from
    /// every source: the budget's picks and anything else that marked a probe
    /// dirty — plus, on a from-scratch build, the placement pass's own captures
    /// of the whole grid. 0 in every mode but the hybrid.
    int  probeCapturesLastFrame = 0;
    /// HOW MANY FRAMES THE MOTION DEFERRAL HELD THE BUDGET, for the life of the
    /// scene (DRAG-1, REFLECT F3). A capture of a box that is still moving is
    /// out of date before it is displayed and the next frame stales it again,
    /// so the spend waits for the content to hold still; the staleness itself
    /// is recorded either way, so the sweep guarantee keeps its shape. A scene
    /// nothing moves in reads 0 for ever; a drag raises it by one per frame.
    unsigned long long probeCapturesDeferred = 0;
    /// How many probes are still stale — owe a capture the budget has not
    /// spent yet. The grid has caught up when this reads 0; it drains at the
    /// resolved budget per frame (probeUpdatesPerFrame).
    int  staleProbes = 0;
    /// The input that last marked the grid stale, and a serial that moves each
    /// time something does (so a reader can tell two events with the same
    /// reason apart).
    GiStaleReason lastStaleReason = GiStaleReason::None;
    unsigned long long staleSerial = 0;
    /// FROM-SCRATCH GI builds (VCT/hybrid arm or an Instant Radiosity re-trace
    /// from setGlobalIllumination or a post-destruction flush) since the scene
    /// was created. A page return, a refresh and an idle frame must never move
    /// it; a mode, quality, grid or bounds change and a destroyed object do.
    unsigned long long rebuilds = 0;

    /// THE MOVEMENT SCAN'S OWN COST (clean-2 lane, 2026-09-13). The renderer
    /// cannot be told that a document node moved — the document writes into the
    /// shared scene graph directly — so it walks every item's world AABB to
    /// find out. That walk used to run on EVERY frame of every probe-lit scene,
    /// still or not (392 / 1961 / 4328 us at 1k / 5k / 10k nodes); it now runs
    /// only on frames where a transform was written
    /// (`Engine::setTransformWriteCounter`).
    ///
    /// `giScans` is cumulative and is the assertable half: the delta over N
    /// STILL frames must be 0, and over N frames with something moving, N.
    /// `giScanMicros` is what the last scan that actually ran cost — 0 on a
    /// skipped frame, which is the whole point.
    unsigned long long giScans = 0;
    double             giScanMicros = 0.0;
    /// EVERY `getWorldAabbUpdated` THIS ENGINE'S OWN GI CODE HAS ASKED FOR,
    /// cumulative. That call walks the node's parent chain and recomputes the
    /// node's whole SIMD block, and there were three separate per-frame walks
    /// making it per item: the movement scan and the two signatures the host
    /// reads to decide whether to re-solve (plus the Forward+ slice walk one
    /// frame in thirty). The acceptance for all four is this counter: its
    /// delta over a STILL frame is 0.
    unsigned long long giAabbReads = 0;

    // ---- PHOTON cascades (PHOTON_SPEC P0) ----------------------------------
    /// ONE live cascade, as BUILT — the counterpart of GiParams::GiCascadeDesc.
    struct CascadeStatus {
        /// Half-extent in metres of the box this cascade covers.
        float halfSize = 0.0f;
        /// Voxel resolution per axis.
        int   resolution = 0;
        /// Metres per voxel (halfSize * 2 / resolution) — the number that says
        /// what this cascade can actually resolve.
        float cell = 0.0f;
        /// Metres of camera travel between re-centres (stepCells * cell).
        float step = 0.0f;
        /// THE NEAR-FIELD RADIUS THIS CASCADE GUARANTEES, in metres:
        /// `halfSize - step - cell` (Types.h's giCascadeGuaranteedRadius and
        /// the rule above it). Inside it the near field is VOXELISED BY THIS
        /// cascade at every moment of any walk — its box always covers that
        /// radius — so the bounce a walker stands in is built at this cell size
        /// and not at the coarser cascade's. It is not a claim about every cone
        /// SAMPLE: a cone aimed outward leaves the box and hands over to the
        /// coarser cascade by construction. Negative means the cascade
        /// guarantees NOTHING — the camera can be outside the box before it
        /// re-centres, which is what every tier but Low did before
        /// CASCADE-STEP-1. The STILL-CAMERA bound: the motion terms are bought
        /// with a cell of slack in the step instead (see the rule). The
        /// assertable form (gi.cascades cases 16a and 16b, and
        /// scripting.e2e.world_modes over both columns).
        float guaranteedRadius = 0.0f;
        /// The world-space centre it is currently built at (quantised to its
        /// own lattice, so it is NOT the camera position).
        Vec3  centre;
        /// How many times this cascade has been (re)voxelised since the arm was
        /// built. At rest it does not move: a still camera scrolls nothing.
        unsigned long long rebuilds = 0;
        /// This cascade is BEHIND the camera and owes a rebuild it has not been
        /// given a frame for (at most one cascade is rebuilt per frame).
        /// A FLAG, not a queue: a rebuild always happens at the CURRENT camera,
        /// so owing two is the same as owing one. Non-zero only while the
        /// camera is outrunning the scheduler.
        int   pending = 0;
        /// How many GI items this cascade's LAST REBUILD voxelised: the ones
        /// inside its box that are big enough to fill half a voxel of it,
        /// re-counted on every rebuild — so it follows the cascade as it
        /// scrolls, and reads 0 for one standing in empty space. A coarse
        /// cascade declines sub-voxel objects — it cannot represent them, and
        /// they are what a whole re-voxelisation spends its time on.
        int   items = 0;
        /// How many GI items this cascade's voxeliser HOLDS — the attach set.
        /// Without an instance budget (`GiParams::cascadeInstanceCap` 0) that is
        /// the whole size-filtered scene and `items` is the part of it this
        /// cascade's box reached; with a budget it is at most the budget, and
        /// the two together say whether the budget is biting and on what.
        int   attached = 0;
        /// CPU milliseconds of that same rebuild (the submission cost on the
        /// frame's own thread). The GPU half is NOT here and cannot be: a
        /// timestamp pair is read back two frames later, so it is reported
        /// where a two-frame-late number belongs — the monitor's `vct.cascadeN`
        /// cacheWork rows (ogre-patch 0027).
        float lastCpuMs = -1.0f;
        /// WHICH MESH LOD LEVELS THIS CASCADE ATTACHED (ATOM stage 1's
        /// hand-off): a histogram over the attach set, `lodLevels[L]` items at
        /// level L, index 0 the authored geometry. Never empty once a cascade
        /// has attached anything, and `{N}` — everything at level 0 — for a
        /// scene of meshes with no baked chain, which is every scene built from
        /// document primitives.
        ///
        /// The level a cascade takes is decided by ITS OWN CELL and by the
        /// mesh's baked error, never by the camera: the LOD bias
        /// (Scene::setLodBias) moves what is DRAWN and must not move this.
        std::vector<int> lodLevels;
        /// HOW MANY TRIANGLES THE ATTACH SET HANDS THIS CASCADE at those levels
        /// — the currency of a voxelisation, since the raster dispatch is sized
        /// by the index count and not by the object count. It is the attach set
        /// (what the voxeliser holds) and not the enclosed set, so it is the
        /// pair of `attached` rather than of `items`, and it is the number the
        /// far-field proxy moves: the same cascade with the LOD chain off reads
        /// the authored total.
        long long voxelTriangles = 0;
        /// HOW MANY COMPUTE DISPATCHES THAT REBUILD COST (ogre-patch 0065).
        ///
        /// The voxeliser groups the instances it holds into BUCKETS by what a
        /// dispatch binds — the vertex format, the index width, whether a
        /// texture pool is needed, and WHICH MATERIAL POOL the material is in —
        /// and issues one dispatch per bucket per octant, each sized by the
        /// whole volume however few instances the bucket holds. So this is the
        /// number that says whether a cascade is paying for its MATERIAL COUNT
        /// rather than for its geometry: a scene that shares materials reads a
        /// handful whatever its size, and one whose every object owns a material
        /// reads one dispatch per pool of them. `voxelTriangles` is the geometry
        /// half of the same rebuild's bill.
        long long voxelDispatches = 0;
    };
    /// The live cascade chain, innermost first. Empty unless
    /// GiParams::cascades built one.
    std::vector<CascadeStatus> cascades;
    /// THE FAR-FIELD PROXY, AS APPLIED: whether the cascades are voxelising the
    /// baked LOD levels (`GiParams::cascadeVoxelLod` met with the engine's
    /// run-wide `JAHSHAKA_NO_CASCADE_LOD` diagnostic latch). It is here so the
    /// arm a measurement is on has a NAME — a switch nothing can report is a
    /// switch nobody can trust. True is the default and the shipped arm.
    bool cascadeVoxelLod = true;
    /// The chain is WANTED but has not been built, because no view has tracked a
    /// camera yet — a camera-centred arm is built around the camera and there is
    /// no honest place to put it before one exists. Distinguishes "no view yet"
    /// from "the build failed", which both read as an empty `cascades` list.
    bool cascadesAwaitingCamera = false;
    /// The arm is WANTED but has not been built because an ALBEDO or EMISSIVE
    /// texture it would voxelise is still streaming (BOOTVOX-1). The voxeliser
    /// copies exactly those two slots into its texture pool, so building now
    /// stores the wrong colours and buys a second, full re-voxelisation the
    /// moment the pixels land — which is what the shipped default scene paid on
    /// every boot. The build happens on the frame the last one is resident, and
    /// the wait is BOUNDED (30 deferrals) so a decode that never completes
    /// cannot park GI: after that the arm is built without them and this reads
    /// false again. The other half of `cascadesAwaitingCamera`'s question —
    /// "the chain is empty, why?" — and true in the single-volume arm too.
    bool awaitingVoxelTextures = false;
    /// WHICH COLUMN OF THE TIER TABLE THIS CHAIN WAS BUILT FROM (V1-RIG item 4):
    /// true when the view driving GI is the HEADSET'S, so the chain is the VR
    /// profile's (see GiViewProfile). It is a reading and not a request: the
    /// profile follows the driver, and entering or leaving a session rebuilds
    /// the chain once because the table changed under it.
    bool cascadeProfileVr = false;
    /// How many whole-chain rebuilds the two DIRTY_ALL guards have forced — a
    /// teleport, or a jump longer than a cascade. Cumulative over the scene's
    /// life: a re-solve of the arm does not reset it, only GI going off does.
    unsigned long long cascadeFullRebuilds = 0;
    /// Cascade rebuilds SKIPPED because the frame's budget (one per frame) was
    /// already spent. Cumulative; it is the queue pressure reading.
    unsigned long long cascadeDeferrals = 0;
    /// HOW MANY INJECTION PASSES THE LAST LIGHT TICK SPENT over the cascade
    /// chain (LAMPREST-2). Re-injecting a chain is one Jacobi iteration of its
    /// coupled radiance — each cascade reads the ones outside it and the volume
    /// it is injecting into — so an AT-REST tick iterates until the answer stops
    /// depending on the state it started from (measured: two passes left 4/255
    /// of that history in the movable-lamp room, three left none, and three,
    /// four and six produce the same picture), while a MOVING tick spends
    /// exactly one, because that answer is replaced a few frames later by
    /// construction. 1 in the single-volume arm, which is not an iteration at
    /// all, and 0 before any injection.
    int chainSweeps = 0;
    /// HOW MANY POST-REBUILD SETTLES this scene has paid (LAMPREST-3): a
    /// cascade rebuild injects one cascade once, over the radiance it held
    /// where it used to stand, so the chain owes an at-rest injection
    /// afterwards — paid on the first frame the rebuild queue is empty, once
    /// per burst of rebuilds. Cumulative over the scene's life; 0 in the
    /// single-volume arm, which has no chain to leave behind.
    long long chainSettles = 0;
    /// Scrolls where MORE THAN HALF of the cascade's volume was new — the
    /// second DIRTY_ALL guard, counted rather than acted on in the whole-rebuild
    /// arm. It is the reading that says whether an incremental (slab-shifting)
    /// arm could save anything at this speed: a scroll that is already
    /// majority-dirty has nothing to shift.
    unsigned long long cascadeDirtyMajority = 0;
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

    /// AERIAL PERSPECTIVE: take the fog's COLOUR from the analytic sky instead
    /// of `colour` (SKY-GPU). The engine's atmosphere component computes a
    /// per-vertex scattering colour for the direction each surface is seen
    /// from, so a distant hill fades into the sky it stands against rather than
    /// into one authored grey — and it changes with the sun, for free, because
    /// it is the same model the sky is drawn with.
    ///
    /// OFF BY DEFAULT, and off is what every scene authored before this had:
    /// `colour` is a value a person picked, and no scene's look changes unasked.
    /// The height layer is unaffected either way (it is ours, and it uses
    /// `colour`); with the atmospheric colour on, the two layers are
    /// deliberately different colours — the distance haze is the sky's, the
    /// ground layer is the author's.
    bool   atmosphereColour = false;
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
/// THE REFLECTION CUTOFF'S FEATHER, and the only copy of it (DRAG-1 round 2,
/// F13; moved here from EnginePrivate.h).
///
/// A scene carries a reflection ROUGHNESS CUTOFF — where a traced or marched
/// reflection stops being worth its cost — and both arms ramp their confidence
/// to zero across a band of this half-width around it: the traced half from full
/// at `cutoff - kRayReflectFeather` to zero at `cutoff + kRayReflectFeather`,
/// the marched half from full at `cutoff - kRayReflectFeather` to zero AT the
/// cutoff. What one gives up the other's fallback takes through the same
/// composite, so a floor whose roughness varies across it crossfades instead of
/// stepping.
///
/// A CONSTANT and deliberately not a second dial: the cutoff says WHERE the
/// technique stops being worth it (content), the feather only says that it stops
/// smoothly (renderer). 0.1 is about two and a half times the +-0.04 that a
/// roughness map's 8-bit quantisation can move a neighbouring pixel by, so the
/// ramp is always wider than the noise it hides.
///
/// IT IS IN THE PUBLIC HEADER so that anything reasoning about the band reads
/// the shipped number instead of copying it. It was private while only the
/// renderer used it, and document.material_defaults — which asserts that the
/// unauthored material's roughness CLEARS the band — had to carry a second 0.1
/// that nothing would have updated.
constexpr float kRayReflectFeather = 0.1f;

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
    /// THE MIRROR'S TARGET IS THE CHAIN'S TARGET (DRAG-1, RENDER_AUDIT ON-3).
    /// A planar reflection is a SCENE RENDER with no tonemapper of its own, so
    /// its render target has to be able to hold what the scene emits: with the
    /// main chain at RGBA16F (`PostFxDesc::hdr`) and the mirror at 8-bit sRGB,
    /// every radiance above 1.0 inside a mirror was CLIPPED and every dark
    /// reflection carried 8-bit steps the same surface does not show outside
    /// the mirror — banding by construction, and the reason a bright window or
    /// a lamp reflected flat white. It follows the chain rather than being its
    /// own dial for the same reason the probe captures' HDR follows the quality
    /// row: two switches for one picture is one switch too many. Costs 2x the
    /// reflection RTT's memory (a 1024-square slot: 4 MB against 2).
    bool     hdr = false;
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

/// WHAT THE HARDWARE RAY-QUERY TIER IS DOING (PHOTON_SPEC §7 R1).
///
/// The tier keeps a ray-traceable copy of the scene — one bottom-level
/// acceleration structure per mesh, one top-level structure over the instances
/// — built from Ogre's OWN vertex and index buffers (ogre-patches 0038/0039)
/// and recorded into the frame's command buffer. Nothing consumes it yet: R2
/// (probe visibility), R3 (sun contact shadows) and R5 (reflections) are the
/// consumers, and each of them reads the SAME structure.
///
/// `available` is the DEVICE's answer (VK_KHR_acceleration_structure +
/// VK_KHR_ray_query enabled at vkCreateDevice) and cannot be changed by
/// anything but the hardware and the driver; `enabled` is ours — the no-rays
/// switch, which is how a machine WITH rays renders the picture a machine
/// without them gets, so every ray-consuming suite can assert both. Off, every
/// other field reads zero.
struct RayQueryStatus {
    /// The device has the extensions and the features (never true on macOS:
    /// MoltenVK exposes neither, SPECS/research/MOLTENVK_RAY_QUERY_2026-09-14.md).
    bool available = false;
    /// ...and we are using them. False with `available` true is the PROCESS
    /// switch in force (Engine::setRayTracing / --no-ray-query /
    /// JAHSHAKA_NO_RAY_QUERY=1). It is engine-wide: what one SCENE resolves to
    /// is `Scene::rayTracingResolved()`, which ANDs this with the project's own
    /// state (iris::Scene::rayTracing, the World panel's row).
    bool enabled = false;
    /// Bottom-level structures held — one per unique mesh in the traced set.
    int  blasCount = 0;
    /// Instances in the top-level structure: the traced set's size. It is NOT
    /// the scene's Item count — editor helpers, backdrops, the sun disc,
    /// overlay-queue objects, SKINNED Items (they would trace at bind pose
    /// until R4) and alpha-tested ones (no any-hit without ray-tracing
    /// pipelines) are all out.
    int  instances = 0;
    /// Triangles in the bottom-level structures (unique geometry, not
    /// instanced).
    int  triangles = 0;
    /// Bytes of acceleration structure resident: the bottom level after
    /// compaction plus the top level.
    unsigned long long blasBytes = 0;
    unsigned long long tlasBytes = 0;
    /// GPU milliseconds of the LAST top-level build or refit, read back from a
    /// timestamp pair several frames later (never with a wait on the frame
    /// thread). -1 until one has been measured.
    float tlasMs = -1.0f;
    /// GPU milliseconds of the last batch of bottom-level builds, same reading.
    /// -1 until one has been measured; a still scene never rebuilds one.
    float blasMs = -1.0f;
    /// CPU milliseconds THE INSTANCE WALK cost — only the walk that writes the
    /// transforms into the mapped buffer, not the command recording around it.
    /// This is the number that scales with instance count and the one a budget
    /// is kept on.
    float gatherMs = -1.0f;
    /// True when the last top-level update was a REFIT rather than a full
    /// rebuild. The default is a rebuild (NVIDIA's own guidance for a TLAS;
    /// 0.5 ms at 8k instances buys the better tree); the refit is the
    /// optimisation behind the transform-epoch gate.
    bool lastWasRefit = false;
    /// Cumulative counters over the scene's life: how many times the top level
    /// was rebuilt, refitted, and how many bottom-level structures were built.
    unsigned long long tlasBuilds = 0;
    unsigned long long tlasRefits = 0;
    unsigned long long blasBuilds = 0;
    /// RAY-TRACED REFLECTIONS (PHOTON_SPEC §7 R5). True when this scene's
    /// drawn views are tracing reflections — which needs `enabled`, a view
    /// whose SSR row is on (High and Epic; the SSR contract, not a second
    /// setting) and a chain that carries the SSR textures. With it false the
    /// reflections are the screen-space march alone, which is the picture this
    /// renderer drew before R5 existed.
    bool reflect = false;
    /// How many rays the last frame traced for reflections — one per pixel of
    /// the trace's own resolution (full at Epic, one in four at High), before
    /// the shader's own gates (the sky, the roughness band, a pixel the march
    /// already answered) decline most of them.
    int  reflectRays = 0;
    /// GPU milliseconds of that dispatch, read back from a timestamp pair
    /// several frames later and never with a wait. -1 until measured.
    float reflectMs = -1.0f;
};

// ---------------------------------------------------------------------------
// VR (SPECS/VR_SPEC.md v3 phase 2). The whole surface is Ogre-free AND
// OpenXR-free: nothing below names a runtime type, so a host compiled against
// this header needs no OpenXR headers and a build without the loader still
// links (every call answers "unavailable").
// ---------------------------------------------------------------------------

/// Whether this process may talk to an OpenXR runtime at all (EngineConfig::vr).
///
/// `Disabled` is BIT-IDENTICAL to an engine that has never heard of VR: no
/// loader is opened, no XrInstance is created, and — the part that matters —
/// Ogre creates its own VkInstance and VkDevice exactly as it always did. That
/// is the constraint of VR_SPEC §0: "without a headset the tool is today's
/// tool, unchanged", and it is why the desktop selftest hash cannot move for
/// VR work.
///
/// `IfAvailable` asks the loader once, at boot, BEFORE the render system is
/// loaded — because on the `XR_KHR_vulkan_enable2` route the RUNTIME creates
/// the Vulkan instance and device the engine then runs on, and Ogre reads
/// `external_instance` in the render system's constructor. A failure at any
/// step (no loader, no manifest, no runtime, no headset, a device the runtime
/// refuses) is NOT an error: the reason is recorded in VrInfo::reason, the
/// plain boot continues, and vrAvailable() answers false for the life of the
/// process. VR CAPABILITY IS FIXED AT BOOT — plugging a headset in later needs
/// a restart, because WiVRn only writes its runtime manifest on connect.
enum class VrMode {
    Disabled = 0,
    IfAvailable
};

/// The OpenXR session's lifecycle, as the runtime reports it
/// (XrSessionState, one for one, plus the two states that are ours).
///
/// `Unavailable` = no session exists and none can (vrAvailable() false).
/// `Lost` = the runtime or the device went away mid-session; the session is
/// ended cleanly and cannot be restarted in this process (VR_SPEC §7 item 9 —
/// at this pin a lost EXTERNAL device is unrecoverable by construction).
enum class VrState {
    Unavailable = 0,
    Idle,            ///< a session object exists, the runtime has not said Ready
    Ready,           ///< xrBeginSession has been called
    Synchronized,    ///< the runtime is consuming frames; nothing is displayed
    Visible,         ///< displayed, not focused (the dashboard is up)
    Focused,         ///< displayed and receiving input
    Stopping,        ///< xrEndSession pending
    Lost
};

/// What the desktop shows while a session runs (VrConfig::mirror).
enum class VrMirrorMode {
    None = 0,
    Left,     ///< the left eye's half of the both-eyes target — the default
    Right,
    Both      ///< both halves, squeezed into the mirror's own aspect
};

/// What the runtime is and what it wants — filled once at boot (the identity
/// half) and completed when a session begins (the size/refresh half, which
/// needs no session on any runtime measured but is reported from one place).
struct VrInfo {
    bool        available = false;
    /// Why not, when `available` is false: the loader's or the runtime's own
    /// failure, in words, at the step it happened. Empty when available.
    std::string reason;
    std::string runtime;        ///< XrInstanceProperties::runtimeName ("Monado(XRT) ...")
    std::string runtimeVersion; ///< "M.m.p" of XrInstanceProperties::runtimeVersion
    std::string system;         ///< XrSystemProperties::systemName ("Meta Quest Pro on WiVRn")
    /// The OpenXR version the instance was created at — negotiated, never
    /// assumed: 1.1 is asked for and 1.0 is the retry (VR_SPEC §0, the Oculus
    /// audit: Meta's PC runtime has no 1.1 conformance).
    unsigned    apiMajor = 0, apiMinor = 0;
    /// The runtime's RECOMMENDED per-eye size. Never ours to pin: Monado's
    /// null compositor says 320x240 and its xcb one 896x1007; the Quest Pro
    /// over WiVRn says 2160x2376.
    unsigned    eyeWidth = 0, eyeHeight = 0;
    /// From `predictedDisplayPeriod` of the first frame (60.0 on the simulated
    /// HMD, 90.0 on the Quest Pro over WiVRn). 0 until a session has pumped.
    float       refreshHz = 0.0f;
    /// "stage" (a FLOOR origin — what a scene authored with the ground at
    /// y = 0 needs) or "local" (the runtime offers no stage; the head sits
    /// where the wearer was when the session began). Phase 1b measured why
    /// this matters: in LOCAL the Quest Pro's eyes were at y = -0.70 m, under
    /// the floor. Empty until a session has been created.
    std::string space;
    bool        visibilityMask = false;  ///< XR_KHR_visibility_mask is advertised
    bool        depthLayer = false;      ///< XR_KHR_composition_layer_depth is advertised
};

// (MOVED UP FROM BESIDE `VrStatus` BY STAGE 3, VR-HANDS-1: a profile path is now
//  a field of a HAND — `VrHandState::profile` — because the runtime binds one per
//  hand and a wearer may hold a controller in one and nothing in the other, so the
//  type has to be declared before the hand is. Nothing about it changed.)
/// AN INTERACTION PROFILE'S PATH, WITHOUT A HEAP (VR-INPUT-1E-FIX finding 8).
///
/// `VrStatus` is COPIED several times per frame — every host reads it by value
/// (`const VrStatus st = engine->vrStatus()`), the mirror keeps its own copy,
/// and at ninety frames a second that was three or four small allocations a
/// frame for one string nobody edits. A fixed array makes the whole status
/// trivially copyable, which is what a per-frame value type should be.
///
/// SIXTY-FOUR BYTES IS THE WHOLE OPENXR NAMESPACE with room to spare: the
/// longest profile path any runtime can bind here is
/// `/interaction_profiles/microsoft/motion_controller` (49) and the registry's
/// longest is 57. A longer one is TRUNCATED rather than dropped — the string is
/// diagnostic and a prefix still names the vendor — and always terminated.
///
/// It converts to `std::string` implicitly so the one place that hands the path
/// out of the engine (the `vr.state()` verb) is unchanged: the allocation
/// happens THERE, once per call, instead of on every frame's copy.
struct VrProfileName {
    char text[64] = { 0 };
    bool empty() const { return text[0] == '\0'; }
    const char *c_str() const { return text; }
    /// Is `needle` somewhere in the path? (Which model to draw is decided this
    /// way: a Touch controller is any path with `touch_controller` in it.)
    bool contains(const char *needle) const {
        return needle && *needle && std::strstr(text, needle) != nullptr;
    }
    /// Does the path START with `prefix`? (`/interaction_profiles/` — "the
    /// runtime bound something real".)
    bool startsWith(const char *prefix) const {
        if (!prefix) return false;
        const size_t n = std::strlen(prefix);
        return std::strncmp(text, prefix, n) == 0;
    }
    void assign(const char *s) {
        if (!s) { text[0] = '\0'; return; }
        std::strncpy(text, s, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
    }
    void clear() { text[0] = '\0'; }
    VrProfileName &operator=(const char *s) { assign(s); return *this; }
    operator std::string() const { return std::string(text); }
};

/// ONE LOCATED THING, IN WORLD SPACE — the rig already applied (phase 4).
///
/// `valid` is the runtime's own answer for THIS frame: a controller that is
/// switched off, out of the tracking volume or simply not held reports nothing,
/// and a host that drew a marker at the origin for it would be inventing a
/// hand. Unlike VrStatus::posesValid (which latches the last HEAD pose because
/// a locomotion rule must not stutter on one skipped frame) this goes false the
/// moment the runtime stops answering: a proxy that is not there must not be
/// drawn, and there is nothing to keep walking.
struct VrPose {
    bool valid = false;
    Vec3 position;
    Quat rotation;
};

/// WHICH HAND, and what indexes VrStatus::hands.
enum VrHand : unsigned { VrHandLeft = 0, VrHandRight = 1, VrHandCount = 2 };

// ---------------------------------------------------------------------------
// BARE HANDS (SPECS/VR_INPUT_SPEC.md §7, phase 4b stage 3). Everything in this
// block is a PURE RULE over numbers and a profile path — no OpenXR type, no
// engine state — so the session applies it, the host's drawer applies the same
// one, and `vr.grab_maths` asserts all of it with no runtime, no display and no
// headset (which is the only way a sign or a threshold in here is ever seen to
// be wrong before somebody is wearing it).
// ---------------------------------------------------------------------------

/// IS THIS PROFILE A HAND RATHER THAN A CONTROLLER?
///
/// Both hand profiles in the OpenXR registry carry `hand_interaction` in their
/// path — `/interaction_profiles/ext/hand_interaction_ext` (the one this engine
/// suggests) and `/interaction_profiles/microsoft/hand_interaction` (which some
/// runtimes advertise; if one ever binds it, it is still a hand and the same
/// answers are right). A controller profile never does.
///
/// WHAT IT DECIDES: the press thresholds (a pinch is not a trigger, below), the
/// MANIPULATION FRAME (a pinch point is not a palm, below), and which model a
/// host draws for that hand (the vendored controller, or the wearer's own
/// joints).
inline bool vrIsHandProfile(const char *profile) {
    return profile && *profile && std::strstr(profile, "hand_interaction") != nullptr;
}

/// THE PRESS THRESHOLDS — TWO PAIRS, BECAUSE A PINCH IS NOT A TRIGGER.
///
/// The VALUE is always the runtime's (it decides what "pinched" means from its
/// own tracking, and on hands it is a confidence rather than a travel); the
/// HYSTERESIS is ours, and it exists so that a hand resting on the threshold
/// cannot chatter a gesture on and off at ninety frames a second.
///
/// A TRIGGER travels a centimetre under a finger that is already braced against
/// it, so 0.5 up / 0.4 down is a tenth of its travel and enough.
/// A PINCH has no detent, no spring and no end stop: the wearer's fingers drift
/// through the middle of the range on the way to anything else they do, and a
/// runtime's pinch value wanders while two fingertips are merely close. So the
/// band is WIDE — 0.7 to press, 0.3 to release — which costs a pinch a little
/// more commitment and buys the wearer a hold that does not let go while they
/// move their hand. (The numbers are the spec's §7 and are the one thing in
/// this lane that only the owner's headset can judge; they live here, once.)
inline constexpr float kVrTriggerPressOn = 0.5f, kVrTriggerPressOff = 0.4f;
inline constexpr float kVrPinchPressOn = 0.7f, kVrPinchPressOff = 0.3f;

/// ONE PRESS, FROM ONE VALUE AND THE LAST ANSWER. `latch` is the previous
/// frame's press for that control; the return is this frame's.
inline bool vrPressLatched(float value, bool latch, float on, float off) {
    return latch ? (value > off) : (value >= on);
}

/// HOW MANY JOINTS A TRACKED HAND HAS (`XR_HAND_JOINT_COUNT_EXT`), and the
/// ORDER they arrive in — which is the extension's own enumeration and is
/// relied on by the bone table below:
///
///   0 palm · 1 wrist · 2-5 thumb (metacarpal, proximal, distal, tip)
///   6-10 index · 11-15 middle · 16-20 ring · 21-25 little
///   (each finger: metacarpal, proximal, intermediate, distal, tip)
enum : unsigned { kVrHandJointCount = 26 };

/// THE HAND'S SKELETON AS SEGMENTS — what a drawer draws.
///
/// TWENTY-FOUR BONES: the four finger chains (4 each), the thumb's three, and
/// the FAN from the wrist out to the five metacarpals, which is the palm. The
/// palm JOINT (0) is deliberately not in it: it is the middle of the hand, not
/// an end of anything, and it is what a controller's grip pose stands in for.
struct VrHandBone { unsigned char from, to; };
enum : unsigned { kVrHandBoneCount = 24 };
inline constexpr VrHandBone kVrHandBones[kVrHandBoneCount] = {
    // the palm: the wrist out to each metacarpal
    { 1, 2 }, { 1, 6 }, { 1, 11 }, { 1, 16 }, { 1, 21 },
    // the thumb (no intermediate joint)
    { 2, 3 }, { 3, 4 }, { 4, 5 },
    // index, middle, ring, little
    { 6, 7 }, { 7, 8 }, { 8, 9 }, { 9, 10 },
    { 11, 12 }, { 12, 13 }, { 13, 14 }, { 14, 15 },
    { 16, 17 }, { 17, 18 }, { 18, 19 }, { 19, 20 },
    { 21, 22 }, { 22, 23 }, { 23, 24 }, { 24, 25 },
};

/// WHERE ONE BONE'S SEGMENT GOES — the ONE definition, because there are two
/// writers (a running session, inside its own frame, and the host's mirror for
/// a frame no session drew — the same arrangement as the controller proxies).
///
/// THE GEOMETRY IS A UNIT SEGMENT DOWN -Z, exactly like the pointing ray's
/// (SceneMirror::syncVrProxies): the node is stood at `from`, turned so that
/// its own -Z runs to `to`, and scaled along Z to the distance between them. So
/// a hand that moves every frame rebuilds NO geometry at all — twenty-four
/// transforms per hand and one mesh in the whole scene — which is why this is
/// not a line strip per finger (a strip's points ARE its vertex buffer, and a
/// per-frame rebuild is a create-and-destroy of a GPU buffer ninety times a
/// second; see XID-1 for what recycled Vulkan blocks cost).
///
/// A DEGENERATE BONE (two joints at the same place, which a runtime reports
/// while a hand is half-tracked) answers false and is drawn by nobody, rather
/// than becoming a normalisation by zero.
inline bool vrBoneTransform(const Vec3 &from, const Vec3 &to,
                            Vec3 &position, Quat &rotation, Vec3 &scale) {
    const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(len > 1e-5f)) return false;
    const float ix = dx / len, iy = dy / len, iz = dz / len;
    // The shortest rotation taking -Z onto the bone's direction. `v = -Z x d`,
    // `w = 1 + (-Z . d)`, then normalise — the standard two-vector form, with
    // the antiparallel case (d == +Z, w == 0) spelled out rather than left to
    // divide by zero: any axis perpendicular to Z will do and X is one.
    // u = (0, 0, -1), v = d:  u x v = (v.y, -v.x, 0),  u . v = -v.z.
    const float dot = -iz;
    float qx = iy, qy = -ix, qz = 0.0f, qw = 1.0f + dot;
    if (qw < 1e-6f) { qx = 1.0f; qy = 0.0f; qz = 0.0f; qw = 0.0f; }
    const float n = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    position = from;
    rotation = Quat(qx / n, qy / n, qz / n, qw / n);
    scale = Vec3(1.0f, 1.0f, len);
    return true;
}

/// ONE HAND'S WHOLE INPUT, FOR ONE FRAME (SPECS/VR_INPUT_SPEC.md §2, phase 4b
/// stage 1). Two poses and the controls, in the frame a caller can reason in.
///
/// THE TWO POSES ARE DIFFERENT QUESTIONS, and both are the runtime's answers
/// rather than ours. `grip` is where the hand IS — the middle of the fist round
/// the controller, which is where a model or a marker is drawn — and `aim` is
/// where the hand POINTS: a runtime-authored ray whose origin sits forward of
/// the fist and whose direction is the controller's own pointing axis, which on
/// every headset measured is NOT the grip's -Z. A pointer built out of the grip
/// pose is a pointer that disagrees with the wearer's own hardware.
///
/// THE RAY IS -Z OF `aim.rotation`, by the OpenXR convention this whole engine
/// keeps: origin = aim.position, direction = aim.rotation * (0, 0, -1).
///
/// EVERY CONTROL IS A VALUE PLUS ITS EDGE-FREE PRESS, never a wall-clock or a
/// latch: `select`/`grab` are the analogue 0..1 (a bool input reads 0 or 1),
/// and the `*Pressed` booleans are those values through ONE threshold with
/// hysteresis (0.5 on the way up, 0.4 on the way down) so a trigger resting on
/// the line does not chatter. A host that wants an EDGE compares two frames —
/// which it must do anyway, because a frame the runtime skipped has no press in
/// it at all.
///
/// `valid` is this frame's answer for the whole hand: a controller switched off,
/// put down or out of the tracking volume reports nothing, and everything below
/// is then at its zero. Like VrPose::valid and unlike VrStatus::posesValid it
/// never latches.
struct VrHandState {
    bool   valid = false;        ///< the runtime reports this hand this frame
    VrPose aim;                  ///< world space through the rig: the pointing ray
    VrPose grip;                 ///< world space through the rig: where the model is drawn
    /// THE MANIPULATION FRAME — WHERE THE HAND HOLDS SOMETHING (stage 3,
    /// VR_INPUT_SPEC §7). It is the frame a grab is measured in: the object is
    /// captured relative to it and follows it rigidly.
    ///
    /// ON A CONTROLLER IT IS THE GRIP, and nothing changes: the wearer's fist
    /// is round the controller and the thing they pick up is in that fist.
    /// ON BARE HANDS IT IS THE PINCH POSE (`pinch_ext/pose`) — the point where
    /// the fingertips meet — because the grip of a hand is the PALM, and an
    /// object welded to the palm while the wearer pinches hangs several
    /// centimetres off the fingers that are supposed to be holding it, rotating
    /// about the wrong point every time they turn their hand.
    ///
    /// WHICH ONE IS CHOSEN BY THE BOUND PROFILE (`vrIsHandProfile(profile)`),
    /// and the choice is made ONCE, in the engine, so nothing above the
    /// boundary has to know whether the wearer is holding anything. It is never
    /// invalid while `grip` is valid: with no pinch pose to be had it IS the
    /// grip.
    VrPose manipPose;
    float  select = 0.0f;        ///< trigger 0..1
    bool   selectPressed = false;
    float  grab = 0.0f;          ///< squeeze 0..1
    bool   grabPressed = false;
    bool   menuPressed = false;
    float  stickX = 0.0f, stickY = 0.0f;   ///< -1..1
    bool   stickPressed = false;
    /// WHAT THE RUNTIME HAS BOUND FOR THIS HAND, as its own path (stage 3).
    ///
    /// PER HAND, BECAUSE THE RUNTIME BINDS PER HAND: WiVRn binds
    /// `ext/hand_interaction_ext` while the controllers are down and
    /// `oculus/touch_controller` for a hand that picks one up — each hand on
    /// its own, so a wearer really can hold a controller in one hand and
    /// nothing in the other (hands and controllers are alternative MODES on
    /// WiVRn, never simultaneous per hand: VR_INPUT_SPEC §1.5).
    ///
    /// It is what decides the press thresholds and the manipulation frame
    /// above, which model a host draws for this hand, and — because a mode
    /// change means the wearer's hand physically changed shape mid-gesture —
    /// when a gesture on this hand is CANCELLED (a host compares it with the
    /// previous frame's; the engine keeps no edge of its own).
    ///
    /// Empty = the runtime has bound nothing for this hand: no controller, bare
    /// hands the runtime does not track, or an unfocused session.
    VrProfileName profile;
    /// IS THIS HAND'S SKELETON BEING TRACKED this frame (`XR_EXT_hand_tracking`
    /// answered `isActive` and located its joints)? The joints themselves are
    /// NOT in this struct — fifty-two poses would be two kilobytes on a value
    /// every host copies several times a frame — they are fetched on demand
    /// through `Engine::vrHandJoints`.
    bool   jointsTracked = false;
    /// A TEST HOOK WROTE THIS SAMPLE (Engine::vrInjectInput), not the runtime.
    /// Reported so nothing downstream can mistake an injected gesture for a
    /// wearer's — a smoke in a headset that ever sees this true is looking at a
    /// stale injection, which is exactly what the refusal rule prevents.
    bool   fromInjection = false;
    // (THERE IS NO PER-HAND `focused` HERE — VR-INPUT-1E-FIX, the second read
    // of the Studio half. Focus is a property of the SESSION: the runtime takes
    // it away for the whole application, never for one hand, so the same bit
    // was written to both hands and every consumer had to fold two copies of
    // one truth back together — and a host that OR-ed them could miss a cancel
    // (an unfocus on one hand while a stale sample on the other still said
    // focused). It lives on `VrStatus::inputFocused` now, once.)
};

/// The engine's own `VrHandState` exists — what the Studio side's guarded
/// mirror of this struct (src/modules/vr/vrinteraction.h) compiles out
/// against, so the two halves of phase 4b stage 1 never define it twice.
#define JAH_ENGINE_HAS_VRHANDSTATE 1

/// THE CONTROLLER'S RAY AND ITS HIT, AS THE HOST COMPUTED THEM
/// (Engine::setVrRay; VR_INPUT_SPEC §3). The engine DRAWS this and nothing
/// else: the picking is the document's (one picker, `iris::picking`), the
/// selection rules are the host's, and the two helper nodes the mirror owns are
/// placed from these numbers inside the frame that draws them.
///
/// `origin` and `dir` are WORLD space; `dir` need not be normalised. With
/// `hit` the line stops at `hitPoint` and a small marker is drawn there;
/// without it the line runs to `length` (or ten metres).
struct VrRayState {
    bool visible = false;
    Vec3 origin;
    Vec3 dir;
    bool hit = false;
    Vec3 hitPoint;
    /// How far the line runs when nothing was hit, metres. 0 = ten metres.
    float length = 0.0f;
    /// WHICH HAND IT BELONGS TO (VrHandLeft/VrHandRight), or -1 for "take the
    /// origin and direction exactly as given".
    ///
    /// WHY IT IS WORTH A FIELD. A host computes this ray from the pose it last
    /// HEARD, which is a frame or two old — the same lag that made the engine
    /// place the controller proxies itself (Scene::setVrProxyNodes). Naming the
    /// hand lets the session re-anchor the line to THIS frame's aim pose while
    /// keeping the host's own length, so the ray leaves the wearer's hand
    /// exactly where the model is and only its far end is as old as the pick.
    int hand = -1;
};

/// A session's parameters. Everything here is the HOST's choice; nothing is
/// persisted by the engine.
struct VrConfig {
    VrMirrorMode mirror = VrMirrorMode::Left;
    /// Metres of world per metre of room. 1 = life size.
    float worldScale = 1.0f;
    /// FOR MEASUREMENT ONLY (VR_SPEC §5 phase 2's "at 2160x2376 per eye"): render
    /// each eye at this size instead of the runtime's recommendation. The XR
    /// swapchains still use the runtime's size, so the copy scales — which is
    /// exactly what makes it a measurement of the RENDER and not of the runtime.
    /// 0 = use the runtime's recommendation (the product path).
    unsigned overrideEyeWidth = 0, overrideEyeHeight = 0;
    /// HOW MANY STEREO WARM-UP FRAMES THE SESSION RENDERS BEFORE ITS FIRST
    /// COMMITTED EYE FRAME (lane VR-WARMUP-1). 0 = none.
    ///
    /// THE MEASUREMENT THIS EXISTS FOR (spikes/vr-warmup-1): the session's
    /// SECOND frame — the first one the runtime asks a picture of — cost
    /// 1,179 ms cold and 89 ms warm on the Grand Showroom (383 / 20 on the
    /// default scene), all of it inside `engine.record`'s two `Jahshaka opaque`
    /// passes with the GPU idle: Hlms permutations generated, shaders compiled
    /// and pipelines built ON THE FRAME THREAD, at the moment the wearer is
    /// first shown the world. At 62.5 Hz that is 73 repeated headset frames.
    ///
    /// WHY IT CANNOT BE PAID ANYWHERE ELSE. The desktop's own warm-up does not
    /// cover it (measured: 783 ms on the second VR frame after 200 mono frames
    /// in the same process) because the INSTANCED-STEREO permutation set is its
    /// own — `hlms_instanced_stereo` is a pass property, so every shader the
    /// eyes need is a different shader from the one the desktop compiled; and
    /// a fully warm microcode cache still paid 498 ms when the EYE SIZE changed
    /// (two permutations whose generated source depends on the target). So the
    /// warm-up renders the SESSION's own chain, stereo, at the SESSION's eye
    /// size: the only shape that covers both.
    ///
    /// WHAT A WARM-UP FRAME IS: a real frame of the session's view, from the
    /// rig's origin, through a deliberately wide frustum (so culling keeps
    /// nothing back), with NO XR frame open — nothing is submitted to the
    /// runtime and nothing is mirrored. Frame k faces the origin's heading
    /// turned by k*180 degrees, so two frames see the whole room.
    ///
    /// TWO by default: one forward, one back. The cost moves to the start of
    /// the session, where the runtime is still showing its own picture and
    /// asking for none (VrStatus::warmUpMs says what it cost).
    unsigned warmUpFrames = 2;
    /// DOES THE WEARER SEE THE EDITOR'S FURNITURE — the ground grid, the light
    /// and camera icons, the selection outline, the gizmo (owner, 2026-09-17)?
    ///
    /// IT FOLLOWS THE HOST MODE, and the host is the only one who knows which
    /// it is. The EDITOR's VR preview passes true: the whole point of that mode
    /// is to stand inside the scene and watch the editor work, and a wearer who
    /// cannot see what is selected or where the grid is cannot author. The
    /// PLAYER passes nothing and gets this default: a play session shows no
    /// furniture, exactly as the desktop Player shows none.
    ///
    /// The VR CONTROLLER proxies are NOT governed by it — they carry their own
    /// channel (kVrHelperBit) and are drawn in every VR eye, both modes, like
    /// Unreal's. Off by default here, so a host that says nothing gets the
    /// conservative answer.
    bool helpers = false;
    /// THE REFLECTION ROW THE HEADSET RENDERS WITH (lane REFLECT-VR-1), the
    /// same 0/1/2 as `PostFxDesc::ssr` — 0 off, 1 half-resolution, 2 full.
    ///
    /// IT IS THE HOST'S TO PASS, because the row is the PROJECT'S (the World
    /// panel's SSR row, `iris::Scene::ssrMode`, which the mirror pushes into
    /// the desktop view every frame) and this struct is the only channel a
    /// session has to it: the session creates its own View inside the engine
    /// and no mirror ever reaches it. Both Studio hosts pass the project's row,
    /// so the wearer sees the reflections the author sees.
    ///
    /// The SOURCE is not a choice here: a stereo chain never marches in screen
    /// space (see `PostFxDesc::ssrScreenMarch`), so this row buys RAY-TRACED
    /// reflections on a ray-capable machine and NOTHING AT ALL on one without —
    /// `chain::build` declines to build the reflection stage when neither source
    /// can write it, so a machine with no ray queries does not even pay the
    /// prepass and renders exactly what it renders today.
    int ssr = 0;
    /// THE RUNTIME'S HIDDEN-AREA MESH (lane HAM-1): mask out the corners of
    /// each eye that the headset's own lenses never show, so no shading is
    /// spent on them.
    ///
    /// ON BY DEFAULT and there is no host row for it: it costs nothing a wearer
    /// can see (the pixels it removes are behind the lens barrel) and it is
    /// bought from the runtime's own geometry (`XR_KHR_visibility_mask`), so a
    /// runtime that answers no mask simply renders what it rendered before.
    /// The flag exists so a MEASUREMENT can take the other arm in the same
    /// process — the suite's mask-off control and the lane's own A/B — which is
    /// the same reason `overrideEyeWidth` is here.
    bool hiddenAreaMask = true;
    /// DOES THIS SESSION BIND THE WEARER'S BARE HANDS (lane HANDS-SWITCH-1;
    /// the owner, 2026-09-18, joint: bare-hand work is deferred until the
    /// controllers are right, and hands-or-controllers is the AUTHOR'S choice
    /// per project rather than the runtime's per moment)?
    ///
    /// OFF BY DEFAULT, and off means the session has no bare-hand route at all:
    ///   * the `ext/hand_interaction_ext` suggested-binding block is NOT
    ///     offered, so the runtime can never bind a hand profile for either
    ///     hand — a wearer who puts a controller down is left holding nothing,
    ///     which is what "controllers only" has to mean;
    ///   * no `XrHandTrackerEXT` is created, so nothing asks the runtime for
    ///     joints; and
    ///   * no skeleton is reported or drawn, the test-injection route included
    ///     (`Engine::vrHandJoints` answers 0 while such a session is live).
    /// The controllers are untouched by it: the other three binding blocks are
    /// offered exactly as before.
    ///
    /// THE HOST PASSES THE PROJECT'S OWN ROW (`iris::Scene::vrHands`, the World
    /// panel's Hands switch and `world.vr({hands})`); `vr.begin({hands})`
    /// overrides it for ONE session, which is what a suite and a measurement
    /// need, exactly like `hiddenAreaMask` above.
    ///
    /// IT IS READ ONCE, at session creation: suggested bindings are attached to
    /// the session's action sets before its first frame and no runtime can be
    /// asked to rebind them, so there is nothing here for a mid-session change
    /// to act on.
    bool hands = false;
};


/// ONE SUGGESTED-BINDING BLOCK, AS THE RUNTIME ANSWERED IT (stage 3's fix
/// round; `Engine::vrBindingBlocks`).
///
/// WHY PER PROFILE AND NOT JUST A PAIR OF TOTALS. `bindingProfiles` /
/// `bindingProfilesAccepted` say "four offered, four taken", which cannot
/// distinguish a block that bound every path it meant to from one that bound
/// half of them: a path spelled wrong, or a profile that lost an input between
/// pin bumps, takes that hardware's control away SILENTLY and the totals still
/// read 4 of 4. The COUNT is what pins it — the bare-hand block is twelve
/// bindings (two poses, the pinch pose, select, grab, per hand) and a suite
/// asserts that number.
///
/// `bindings` is how many `XrActionSuggestedBinding`s the block carried;
/// `accepted` is whether `xrSuggestInteractionProfileBindings` took it (a
/// runtime refuses a profile it does not know, which is not an error).
struct VrBindingBlock {
    VrProfileName profile;
    unsigned      bindings = 0u;
    bool          accepted = false;
};
/// How many blocks this engine can report (it offers four).
enum : unsigned { kVrBindingBlockMax = 8 };

/// What a live session is doing. Every number is a COUNT or a measured value,
/// never a wall-clock derivation (VR_SPEC §6 flake class (b): count frames,
/// never time, on a loaded box).
struct VrStatus {
    VrState            state = VrState::Unavailable;
    bool               active = false;      ///< a session object exists
    unsigned long long frames = 0;          ///< xrEndFrame calls that succeeded
    unsigned long long rendered = 0;        ///< frames the runtime asked us to draw
    /// The distance between the two located eye positions, metres. 0 before the
    /// session's first frame; through the WARM-UP frames it reads the synthetic
    /// pair's 0.064 (VrConfig::warmUpFrames) until the first real xrLocateViews
    /// — `head.valid` is false for as long as that is so.
    float              ipd = 0.0f;
    unsigned           eyeWidth = 0, eyeHeight = 0;   ///< what the chain renders per eye
    /// THE STEREO WARM-UP (VrConfig::warmUpFrames): how many warm-up frames
    /// this session has rendered, and what they cost in total. `warmUpFrames`
    /// reaching the configured number is the signal that the eyes are being
    /// drawn with everything already built; a caller that reads it as 0 on a
    /// session configured for 2 is looking at a session whose runtime has not
    /// started running yet.
    unsigned           warmUpFrames = 0;
    float              warmUpMs = 0.0f;
    VrMirrorMode       mirror = VrMirrorMode::None;
    float              worldScale = 1.0f;
    /// Whether the per-eye PROJECTIONS differ, i.e. whether the runtime gave
    /// the two eyes different fovs. Monado's simulated HMD does not (both eyes
    /// get one symmetric fov and only the POSES separate them); the Quest Pro
    /// does. A test that asserts "the eyes differ" has to know which it has.
    bool               asymmetricFov = false;

    // ---- THE RIG IN THE WORLD (phase 3, the Player's VR mode) -------------
    /// WHERE THE HEAD IS, IN WORLD SPACE — the rig's origin composed with the
    /// pose the runtime reported, which is the only pose a host can reason
    /// about. A host that moves the wearer (locomotion) needs both halves:
    /// where it put the rig, and where the wearer's head ended up inside it.
    ///
    /// `headRotation` is the FULL head orientation (pitch and roll included);
    /// a locomotion rule that wants a level heading takes the forward vector
    /// and flattens it rather than decomposing Euler angles, which is what the
    /// Player's head-relative fly does.
    Vec3               headPosition;
    Quat               headRotation;
    /// Has the runtime EVER located the views in this session? False until the
    /// first located frame, when the two fields above hold nothing but their
    /// defaults. It deliberately does NOT go false again on a frame the runtime
    /// skips or a moment of lost tracking: the fields then hold the last pose
    /// that WAS located, which is the right thing for a host moving a wearer
    /// who is mid-stride — a locomotion rule that stopped dead on one skipped
    /// frame would stutter, and one that reset to the origin would teleport.
    bool               posesValid = false;
    /// THE RIG'S ORIGIN, AS THE HOST LAST SET IT (Engine::setVrOrigin): the
    /// world position of the reference space's origin (the floor spot the
    /// wearer stands on, in STAGE) and its heading in DEGREES about +Y.
    /// Reported back so a caller can see that the engine took what it was
    /// given rather than trusting its own copy.
    Vec3               origin;
    float              originYaw = 0.0f;
    /// HOW MANY TIMES THE RUNTIME RECENTRED THE ROOM under this session (the
    /// Quest's long-press, a guardian re-setup). Each one moves every pose the
    /// runtime reports discontinuously; the session ABSORBS it into the rig so
    /// the wearer stays where they were standing, and this counts them — a
    /// host that sees it climbing while nobody touched the headset is looking
    /// at a runtime problem, not at its own locomotion.
    unsigned long long spaceChanges = 0;

    // ---- THE HIDDEN-AREA MESH (lane HAM-1) -------------------------------
    /// WHERE THE MASK CAME FROM: "runtime" (the eye's own geometry through
    /// `XR_KHR_visibility_mask`), "off" (the host asked for none —
    /// `VrConfig::hiddenAreaMask` false) or "none" (the runtime has no mask to
    /// give, which is what Monado's simulated HMD used to be read as and what
    /// every runtime without the extension is). Empty with no session.
    std::string        hiddenAreaSource;
    /// THE FRACTION OF EACH EYE THE MASK COVERS, indexed by eye (0 = left),
    /// measured on the geometry the runtime handed over: the sum of its
    /// triangles' areas in the eye's own clip rectangle, which has area 4. It
    /// is the number the saving is computed from (the pixels never shaded), and
    /// it is the RUNTIME'S answer — a Quest Pro and a simulated HMD do not
    /// report the same shape, so a measurement that does not carry this number
    /// cannot be read on another headset.
    float              hiddenAreaFraction[2] = { 0.0f, 0.0f };
    /// ...and how many triangles that was, per eye. 0 = no mask on that eye.
    unsigned           hiddenAreaTriangles[2] = { 0u, 0u };

    // ---- THE HANDS (phase 4) ---------------------------------------------
    /// WHERE THE WEARER'S HANDS ARE, in world space, through the rig exactly
    /// like the head. Indexed by VrHand. POSES ONLY: no buttons, no triggers,
    /// no input actions — those are their own spec, and nothing here reads a
    /// value a user pressed.
    ///
    /// The source is the OpenXR ACTION system (a grip pose on the simple
    /// controller profile, which every runtime maps from whatever the wearer is
    /// actually holding), or the hand-tracking extension's palm joint where the
    /// runtime offers it and no controller answers. A runtime with neither
    /// leaves both invalid for the life of the session, which is not an error:
    /// a headset with no controllers is a supported headset.
    VrPose             hands[VrHandCount];
    /// EVERYTHING EACH HAND IS DOING (phase 4b stage 1, VR_INPUT_SPEC §2): the
    /// two poses and the controls. `input[i].grip` IS `hands[i]` — the same
    /// pose, reported twice because `hands` is what phase 4's hosts read and
    /// the pair is what a gesture needs.
    VrHandState        input[VrHandCount];
    /// DOES THE APPLICATION HAVE INPUT FOCUS? (VR_INPUT_SPEC §5.5; the shape is
    /// VR-INPUT-1E-FIX's — it used to be a bit on every hand.)
    ///
    /// The runtime takes focus away whenever its own dashboard comes up, the
    /// headset comes off the head or another application is talking to the
    /// wearer, and it takes it away for the WHOLE APPLICATION: `xrSyncActions`
    /// answers XR_SESSION_NOT_FOCUSED (a SUCCESS code), every action goes
    /// inactive and every control reads its zero. A host that read that as "the
    /// trigger was released" would COMMIT a gesture the wearer never finished,
    /// so the distinction is reported and a gesture in flight is CANCELLED
    /// rather than committed when this goes false.
    ///
    /// True exactly while the session is FOCUSED. With NO session it is false
    /// (nobody is holding anything) — unless a hand is INJECTED, and then it is
    /// what `Engine::vrInjectFocus` was last given, true by default: a test
    /// that says nothing about focus means "the wearer was there", and one that
    /// injects it false is driving the cancel with no dashboard to raise.
    bool               inputFocused = false;
    /// THE INTERACTION PROFILE THE RUNTIME HAS BOUND, as its own path
    /// (`/interaction_profiles/oculus/touch_controller`), or empty when it has
    /// bound none — which is what a wearer with no controllers, and every
    /// unfocused session, reports.
    ///
    /// It is the runtime's CHOICE out of the profiles we suggested.
    ///
    /// A SUMMARY, DERIVED, NOT A SECOND TRUTH (stage 3): the per-hand answer is
    /// `input[h].profile` and this is the right hand's when it has one, else
    /// the left's — because from stage 3 on the two hands really can differ (a
    /// controller in one hand, bare fingers in the other), and anything that
    /// draws a model or measures a press asks the HAND. This field stays for
    /// what it is good for: naming the session's input in a log, a report or
    /// `vr.state().profile`.
    ///
    /// A FIXED ARRAY, not a `std::string` (VrProfileName's note): this struct
    /// is copied several times per frame and a status copy allocates nothing.
    VrProfileName      profile;
    /// HOW MANY SUGGESTED-BINDING BLOCKS WERE OFFERED, and how many the runtime
    /// ACCEPTED (`xrSuggestInteractionProfileBindings`). A runtime refuses a
    /// profile it does not know (a path it cannot resolve, an extension it does
    /// not have) and that is not an error — but a build whose four blocks all
    /// failed would silently have no input at all, so the counts are reported.
    unsigned           bindingProfiles = 0, bindingProfilesAccepted = 0;
    /// The action set was created, bound and ATTACHED to this session — i.e.
    /// the controller route is live and `hands` can become valid. False means
    /// the runtime refused the actions (or the build has none), which is worth
    /// telling apart from "the controllers are switched off".
    bool               handActions = false;
    /// XR_EXT_hand_tracking is advertised, supported by the system and in use.
    /// It is a FALLBACK, never a replacement: a hand holding a controller is
    /// located by the controller.
    bool               handJoints = false;
    /// WAS THIS SESSION ASKED TO BIND BARE HANDS (`VrConfig::hands`, lane
    /// HANDS-SWITCH-1)? Reported because it is the one thing that explains the
    /// two numbers above being three-of-three rather than four-of-four, and
    /// because "is this project on hands or on controllers" is a question the
    /// owner asks of a running session — `vr.state().hands.enabled`.
    bool               handsEnabled = false;
};

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
    /// HARDWARE RAY TRACING, at boot (PHOTON_SPEC §7 R1). True (the default)
    /// lets the tier come up wherever the device advertises it; false is the
    /// fallback picture a machine without ray tracing gets — the one path a Mac
    /// takes, and the one every ray-consuming suite must be able to run on this
    /// GPU so the fallback is proved on every push.
    ///
    /// FALSE REACHES THE DEVICE, not just this tier. The host sets
    /// JAHSHAKA_NO_RAY_QUERY alongside it, which ogre-patch 0038 reads at
    /// vkCreateDevice, so the process comes up on exactly the instance,
    /// extension list and feature set it would have had if the tier did not
    /// exist. "The picture a machine without ray tracing renders" is therefore
    /// literal and not a manner of speaking — which is what makes the suites
    /// that assert the fallback worth anything.
    ///
    /// THE HOST OWNS THIS ANSWER, and since 2026-09-15 (ledger §425) it is the
    /// DIAGNOSTIC LATCH and nothing else: Studio fills it from
    /// `--no-ray-query` alone. What a PROJECT asks for is a document field
    /// pushed per scene (Scene::setRayTracing / RayTracingMode), because ray
    /// tracing is a property of the project met with a property of the machine
    /// — an application preference beside the document's row would be two
    /// layers that can disagree. Change this one at runtime with
    /// Engine::setRayTracing.
    ///
    /// It is NOT a quality dial: with rays off the tier builds nothing at all,
    /// costs nothing at all, and `giStatus().rayQuery.enabled` reads false.
    bool rayTracing = true;
    /// OPENXR (SPECS/VR_SPEC.md §4.1). Disabled by default and on purpose: the
    /// `IfAvailable` route creates the Vulkan instance and device through the
    /// RUNTIME, which is a different boot, so a host opts in per process (Studio:
    /// `--vr` / JAHSHAKA_VR=1) rather than inheriting whatever manifest the last
    /// headset connection happened to write. See VrMode.
    VrMode vr = VrMode::Disabled;
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

    // ---- the two caches the shader HASH addresses (HLMSBITS-1) -------------
    /// THE NUMBER THAT CRASHED THE EDITOR ON 2026-09-14, now readable while the
    /// session is alive. Every shader is looked up by a 32-bit hash built from a
    /// PASS index and a RENDERABLE index; both caches grow for the life of the
    /// process and neither is ever evicted, so "how full are they" is a real
    /// health reading — and past the field's capacity the indices used to spill
    /// into each other silently. Reported as the WORST Hlms (the largest of PBS,
    /// Unlit, low-level), because one full cache is the problem whichever it is.
    unsigned passCacheEntries = 0;
    unsigned passCacheCapacity = 0;        ///< what the hash's pass field addresses
    unsigned renderableCacheEntries = 0;
    unsigned renderableCacheCapacity = 0;  ///< what the hash's renderable field addresses
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

/// HOW THE AUTOMATIC EXPOSURE'S METER LOOKS AT THE FRAME (EXPOSURE-2) — the
/// per-pixel WEIGHT the log-luminance histogram is built with. A camera's
/// metering pattern, by its own names; ignored under `tonemapFixed`, which
/// measures nothing at all.
///
/// The geometry is a circle IN PIXELS on any window shape, and the numbers
/// below are the whole definition of each pattern — they live here, beside the
/// enum, because they are physics rather than policy, and the shader derives
/// everything else from them (JahHdrMeterBuild_cs).
enum class ExposureMeterPattern : int {
    /// The whole frame, equally. The reference the other two are judged
    /// against, and what the pin's ladder always did.
    Average = 0,
    /// The classic camera default: a Gaussian on the distance from the frame
    /// centre, with a pedestal so the corners still count for something.
    CentreWeighted = 1,
    /// A centre disc of `kSpotAreaFraction` of the frame.
    Spot = 2
};

namespace meter {
/// CENTRE-WEIGHTED: the radius, in units of HALF THE FRAME HEIGHT, at which the
/// weight has fallen to half. 0.5 = half weight halfway to the top edge, which
/// integrates (over the 16:9 rectangle, with the pedestal) to 41 % of the
/// meter's sensitivity inside the central 11 % of the picture and 83 % inside
/// the inscribed full-height circle (the infinite-plane Gaussian gives 40/81;
/// the picture has corners). "Half weight" is the Gaussian term's half — with
/// the pedestal the total weight there is 0.525 of the peak.
constexpr float kCentreWeightedHalfRadius = 0.5f;
/// CENTRE-WEIGHTED: the weight a pixel infinitely far from the centre still
/// carries, as a fraction of the peak. NOT zero on purpose: a meter that
/// ignores the edges of the frame outright cannot see a window opening behind
/// the subject.
constexpr float kCentreWeightedPedestal = 0.05f;
/// SPOT: the fraction of the FRAME AREA inside the disc's half-weight radius.
/// 2.5 % is a 35 mm spot meter (1-5 % is the range real bodies offer); on 16:9
/// it is a disc 23.8 % of the frame height across.
constexpr float kSpotAreaFraction = 0.025f;
}   // namespace meter

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
    /// Hable/Uncharted2). The prerequisite for bloom. The exposure may be
    /// MANUAL (`tonemapFixed`, the editor's default since EXPOSURE-1) or
    /// metered; this flag only says the chain exists.
    bool  hdr = false;
    /// THE CHAIN'S OWN EXPOSURE AXIS — natural log, NOT stops: the multiplier
    /// the frame is scaled by before the tonemap is `e^(exposure - 2)` times
    /// the chain's 1024, so +0.69 is one doubling. The host converts: a
    /// document exposure is in STOPS and `iris::lens::toChain` is the one door
    /// between the two (SceneMirror::applyExposure).
    float exposure = 0.0f;
    /// THE AUTO WINDOW, AND IT IS ON THE METER'S AXIS, NOT THIS ONE
    /// (EXPOSURE-1, lead review): the meter's resolve clamps the measured log
    /// luminance — the percentile-clipped mean of its histogram since
    /// EXPOSURE-2 — to `[7.5 - exposureMax, 7.5 - exposureMin]`
    /// (JahHdrMeterResolve_cs.glsl), so these two bound WHAT THE METER
    /// IS ALLOWED TO READ, not what `exposure` above may become. They are inert
    /// under manual exposure, which has no meter to bound.
    ///
    /// (There is no "set min == max for a fixed exposure" recipe any more — the
    /// doc said so for months and it was never how the pin behaved. Manual
    /// exposure is `tonemapFixed`, an explicit flag with its own code path: the
    /// luminance ladder is replaced by a clear to one constant.)
    float exposureMin = -2.5f;
    float exposureMax = 2.5f;
    /// THE METERING PATTERN and THE PERCENTILE CLIPS — the automatic exposure's
    /// meter, and nothing else (EXPOSURE-2). Read only when `hdr` and not
    /// `tonemapFixed`; they are UNIFORMS on the meter's compute jobs, so
    /// changing one never rebuilds the chain.
    ///
    /// The clips are PERCENTILES of the metered weight, darkest first: the
    /// meter averages the log-luminance of the slice between them and throws
    /// the rest away. 10 and 90 by default. What that buys is a meter a sun
    /// disc, a blown window or a specular firefly cannot pull — they are a few
    /// percent of the weight and they are cut — where a MEAN of logs (the pin's
    /// ladder) had no resistance at all. Kept ordered; a degenerate pair falls
    /// back to the whole frame rather than to no measurement.
    ExposureMeterPattern meterPattern = ExposureMeterPattern::CentreWeighted;
    float meterLowPercent = 10.0f;
    float meterHighPercent = 90.0f;
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
    /// THE DITHER'S OFF SWITCH — A DIAGNOSTIC, NOT A DIAL (lane DITHER-1).
    ///
    /// The final grade quantises a floating-point picture to 8-bit display
    /// codes, and that write is DITHERED: a deterministic, screen-space,
    /// zero-mean offset of at most half a code, so a smooth gradient reads as
    /// noise whose local mean follows it instead of as a staircase of contours
    /// (the owner's "rippling in the ground plane while flying"). It is
    /// correctness, so there is no project row for it and there will not be
    /// one.
    ///
    /// THIS EXISTS SO A TEST CAN RENDER BOTH PICTURES IN ONE PROCESS. Every
    /// arm of hdr.dither is measured against the same binary with this set,
    /// and so is the --engine-selftest hash A/B. `JAHSHAKA_NO_DITHER` in the
    /// environment forces it on process-wide (read once) for the arms that
    /// cannot reach into a description — a rig shot, a selftest run.
    ///
    /// A uniform, not a graph term: setting it rebuilds no workspace.
    bool  ditherOff = false;
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
    /// DOES THE SCREEN-SPACE MARCH CONTRIBUTE (lane REFLECT-VR-1)?
    ///
    /// The `ssr` row above selects TWO things that used to be one: the
    /// reflection's RESOLUTION (half or full) and its SOURCES (the screen-space
    /// march, plus the rays where the machine can trace them). This flag turns
    /// the first source off and leaves the row meaning the resolution alone —
    /// the reflection is then whatever the rays answer, and every pixel they
    /// decline keeps the probe/sky answer it has today.
    ///
    /// WHO SETS IT FALSE, AND WHY IT IS NOT A QUALITY DIAL: a STEREO target
    /// carries two eyes side by side in one texture, and a screen-space march
    /// is a walk through THAT texture — it reads the other eye's pixels across
    /// the seam, reconstructs its positions through one camera for two eyes,
    /// and reprojects its colour history the same way. None of those is
    /// repairable by a threshold. The rays have no such term: a ray is traced
    /// in the world from the eye that owns its pixel. So the VR session asks
    /// for this, and `chain::build` ENFORCES it for any stereo chain whatever a
    /// host asked (OgreChain.cpp) — the flag exists so that a MONO view of the
    /// same picture (the per-eye control `vrEyeScreenshot` renders) can be
    /// asked for the same answer, which is what makes the two comparable.
    bool  ssrScreenMarch = true;
    /// How far a reflection ray may travel, in world units. Beyond this the
    /// march gives up and the pixel falls back to the probe/sky reflection.
    /// Scene-scale dependent: the default suits a room, not a landscape.
    float ssrMaxDistance = 25.0f;
    /// How thick the depth buffer's surfaces are assumed to be, in world units.
    /// A depth buffer records a surface's FRONT and nothing else, so a crossing
    /// is only accepted when the ray passed within this much of it — too small
    /// and reflections drop out behind objects, too large and they smear.
    float ssrThickness = 0.5f;
    /// A straight multiplier on the composite confidence. 1.0 is physical —
    /// the reflection replaces the probe answer where the march is confident.
    float ssrIntensity = 1.0f;
    /// THE REFLECTION ROUGHNESS CUTOFF, in PERCEPTUAL roughness (PHOTON_SPEC
    /// §7 R5; owner, ledger §426). ONE number for BOTH sources of a per-pixel
    /// reflection (lane SSR-3): below it the reflection is MARCHED in screen
    /// space and, on a ray-capable machine, TRACED for whatever the screen
    /// cannot see; above it the reflection probes' own prefiltered photograph
    /// answers. A PER-PROJECT dial and not a renderer constant: above the cutoff
    /// a reflection is a wide lobe and the probe's photograph is a good enough
    /// integral of it, so a ray or a march per pixel buys a blurrier answer for
    /// the same cost — but WHERE that point falls is a property of the content,
    /// not of the renderer.
    ///
    /// THERE USED TO BE A SECOND ONE. `ssrRoughnessCutoff` (0.35) gated the
    /// march, nothing in the document ever wrote it, and it was compared
    /// against a G-buffer channel the march decoded with the pre-ogre-patch-0043
    /// range and never square-rooted — so the band the frame applied was
    /// perceptual 0.581, not 0.35 and not anything a user could read. It is
    /// deleted; the march reads this field (OgreChain::updateSsr).
    ///
    /// THE TRANSITION IS FEATHERED, not a step, and BOTH SOURCES FADE OVER THE
    /// SAME 0.1 (`kRayReflectFeather`, one constant in this header since
    /// lane SSR-3 so the two cannot drift). The ray's confidence runs from full
    /// at `cutoff - feather` to zero at `cutoff + feather`; the march's
    /// roughness ramp runs from full at `cutoff - feather` to zero AT `cutoff`,
    /// because the march is skipped above the cutoff and has nothing there to
    /// fade. So a surface more than a feather below the cutoff keeps its screen
    /// reflection whole, and one whose roughness varies across the cutoff — a
    /// scratched floor — hands the pixel over through the existing confidence
    /// composite with no seam. A constant, deliberately not a second dial.
    ///
    /// It is a UNIFORM, not a graph change: moving it must not rebuild a
    /// workspace (see ChainDesc::sameShape).
    float reflectionRoughnessCutoff = 0.4f;
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

    /// THE FIXED GRADE'S EXPOSURE, AS THE TONEMAPPER'S OWN MULTIPLIER, when the
    /// host already knows it (SS1, 2026-09-13). 0 — the default — means "derive
    /// it from `exposure`", which is the grey-card constant `tonemapFixed` has
    /// always used (OgreChain's fixedInverseLuminance: it substitutes a 0.18
    /// mid-grey for the measurement the auto chain would have made).
    ///
    /// WHY A SECOND WAY TO SAY IT. The grey card is right for a THUMBNAIL, which
    /// is a photograph of content nobody has measured. It is about a stop off in
    /// a bright room, and it is wrong for a SCREENSHOT OF THE EDITOR, which has
    /// a measured exposure a few centimetres away: the on-screen view converged
    /// on one over the last second. `View::measuredExposureScale()` reads that
    /// number back, and this field carries it into the one-shot offscreen view,
    /// which can never converge on its own (it lives two frames). Right AND
    /// deterministic — the same picture every time, because it is a constant.
    ///
    /// Ignored unless `hdr && tonemapFixed`. Never negative; 0 is "unknown".
    float exposureScale = 0.0f;

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

    /// THE HIERARCHICAL DEPTH PYRAMID (SPECS/NANITE_SPEC.md §4.3) — PHOTON
    /// SHARED INFRASTRUCTURE, not a picture effect.
    ///
    /// Builds a closest-depth mip chain of the scene depth once per frame, right
    /// after the opaque pass: mip 0 is the depth buffer, and each level after it
    /// holds the CLOSEST depth of its footprint in the level above, down to 1x1.
    /// A stackless screen-space trace walks it instead of stepping pixel by
    /// pixel — Epic measure the compaction that rides on it at up to a 50%
    /// tracing speedup — and nothing in this engine reads it YET.
    ///
    /// So it is OFF by default and costs exactly nothing while off: no texture,
    /// no pass, no shader. UNLIKE every flag above it, it is NOT cleared on an
    /// offscreen view — an offscreen capture is where a trace will be measured,
    /// and the pyramid changes no pixel of the picture either way.
    bool  hzb = false;

    /// THE offscreen opt-in. Offscreen Views ignore every flag above unless this
    /// is set, because their exact colours are what thumbnails, previews and the
    /// pixel suites assert. Two callers set it, both deliberately: a screenshot
    /// that asked to look like the viewport (`screenshot({postFx:true})`), and
    /// the engine suite, which is the only way to pixel-test the chain at all.
    bool  allowOffscreen = false;

    bool operator==(const PostFxDesc &o) const {
        return hdr == o.hdr && exposure == o.exposure && exposureMin == o.exposureMin &&
               exposureMax == o.exposureMax &&
               meterPattern == o.meterPattern &&
               meterLowPercent == o.meterLowPercent &&
               meterHighPercent == o.meterHighPercent && bloom == o.bloom &&
               bloomThreshold == o.bloomThreshold && bloomKnee == o.bloomKnee &&
               ssao == o.ssao &&
               ssaoScale == o.ssaoScale && ssaoPower == o.ssaoPower &&
               ssaoRadius == o.ssaoRadius && ditherOff == o.ditherOff &&
               smaaPreset == o.smaaPreset &&
               ssr == o.ssr && ssrScreenMarch == o.ssrScreenMarch &&
               ssrMaxDistance == o.ssrMaxDistance &&
               ssrThickness == o.ssrThickness &&
               reflectionRoughnessCutoff == o.reflectionRoughnessCutoff &&
               ssrIntensity == o.ssrIntensity &&
               refractions == o.refractions &&
               distortion == o.distortion &&
               distortionStrength == o.distortionStrength &&
               tonemapFixed == o.tonemapFixed &&
               exposureScale == o.exposureScale &&
               looks == o.looks && hzb == o.hzb &&
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

    /// HOW MANY TIMES `Engine::advanceResources()` HAS RUN in this process
    /// (lane OPEN-FRAMES-1). Monotonic, never reset, and reported here because
    /// the question it answers belongs with the frame counters: "did the
    /// renderer's resource bookkeeping move while nothing was being drawn?"
    ///
    /// Only differences mean anything. A host whose long install runs without
    /// frames proves it is still recycling by watching this rise across the
    /// install — which is exactly what `open.frames` asserts.
    unsigned long long resourceAdvances = 0;
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
    /// The lamp-map cache holds this slot's light (every point/spot map is
    /// cached — ENGINE_CACHE_POLICY_SPEC P2 — unless the view has more lamps
    /// than maps, when the node stays on Ogre's closest-first dynamic sort).
    bool     isCached = false;
    bool     dirty = false;   ///< a cached map scheduled to re-render on the next frame
    bool     pssm = false;    ///< the directional slot (three splits) rather than a focused map
    /// Shadow-node passes the last frame spent on this slot's map(s) in the
    /// counted view — 0 for a cached lamp at rest (8 when it re-renders: a
    /// clear, six cube faces and the copy for a point light).
    unsigned passesLastFrame = 0;
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
    /// The depth atlases' bytes (D32): the view's atlas (atlasWidth x
    /// atlasHeight) PLUS every live planar-mirror slot's (half resolution, the
    /// same focused count since D3 — `reflectAtlasBytes` is that part). The
    /// reflection probes' quarter-resolution atlases, one per shadowed probe,
    /// are `probeAtlasBytes`, not included here. NOT included anywhere: the
    /// point-light cube scratch (R^2 x 6 R32F + depth per instance).
    unsigned long long atlasBytes = 0;
    unsigned long long reflectAtlasBytes = 0;
    unsigned long long probeAtlasBytes = 0;
    /// Every light SLOT the live shadow node holds, in slot order.
    std::vector<ShadowMapInfo> mapped;
    /// Shadow-casting point/spot lights with NO map this frame — the lights
    /// whose shadows are silently missing. Empty is the healthy state.
    std::vector<NodeId> unmapped;
    /// WAS THE LAST RENDERED FRAME MEASURED AT ALL?
    ///
    /// The pass counters below are OPT-IN and the opt-in EXPIRES: asking for a
    /// status attaches a callback per compositor pass per frame, and 120
    /// rendered frames without a read detach it again. So the counters have two
    /// distinct zeroes — "the frame rendered no shadow passes", which is a
    /// measurement, and "nobody was counting", which is not.
    ///
    /// They used to be indistinguishable, and the first read after a quiet
    /// spell reported a hard 0 for a scene plainly casting shadows. A suite read
    /// that as a document write being lost (lane SKY-SMALL's diagnosis); the
    /// only reason it had ever worked was that the World panel's shadow rows
    /// poll the same status on every rebind, so an unrelated UI refresh kept the
    /// counters armed.
    ///
    /// FALSE here means every `*PassesLastFrame` / `*RendersLastFrame` /
    /// `mapsDirtiedLastFrame` field below is UNMEASURED, not zero. Ask again
    /// after one more rendered frame — the first ask is what arms them. The
    /// scripting layer reports them as `null` in that state.
    bool countersMeasured = false;
    /// THE COST READINGS OF THE LAST RENDERED FRAME (counted only while
    /// somebody polls — see Engine::shadowStatus, and `countersMeasured` above,
    /// which says whether these are a reading at all). `shadowPassesLastFrame`
    /// = the shadow-node passes the COUNTED view executed (the first enabled
    /// view with shadows; 0 when none has any), and `cachedMapRendersLastFrame`
    /// how many of those re-rendered a CACHED lamp map: zero at rest, which is
    /// what "renders once" means, measurably.
    unsigned shadowPassesLastFrame = 0;
    unsigned cachedMapRendersLastFrame = 0;
    /// THE CACHE'S SELF-CHECK, cumulative for the session: passes hashed while
    /// the shadow node declared FEWER shadow maps than its own light list
    /// indexes — the state that generates a pixel shader which cannot compile
    /// (ogre-patch 0025 removes its cause; this counts any recurrence). Zero is
    /// the only healthy value, and a suite may assert exactly that.
    unsigned shaderLightMismatches = 0;
    /// The same for the planar mirrors' shadow nodes (every budget slot) and
    /// the reflection probes' (every shadowed probe that captured) — total
    /// passes, and the part spent on point/spot maps. A probe capture renders a
    /// dirty lamp map on its FIRST face and reuses it on the other five.
    unsigned reflectPassesLastFrame = 0;
    unsigned probePassesLastFrame = 0;
    unsigned reflectLampPassesLastFrame = 0;
    unsigned probeLampPassesLastFrame = 0;
    /// The lamp-map cache last frame (ENGINE_CACHE_POLICY_SPEC P2-P5): shadow-
    /// node instances (views, planar slots, probes) whose lamps are cached,
    /// instances left on the dynamic sort because the lamps outnumber their
    /// maps (the v1 over-budget rule), whether the counted view's own node is
    /// caching, and how many lamp maps were marked to re-render.
    unsigned cachedInstances = 0;
    unsigned uncachedInstances = 0;
    bool     viewCached = false;
    unsigned mapsDirtiedLastFrame = 0;
    /// HOW MANY TIMES THIS PROCESS HAS REBUILT THE SHADOW ATLAS, cumulative.
    /// The hitch counter: a rebuild swaps the three shadow-node DEFINITIONS,
    /// which means dropping and recreating EVERY workspace that names one —
    /// every view, every planar-mirror slot, and every reflection probe, whose
    /// GI arm is then built from scratch. It is the single most expensive thing
    /// the shadow system does and it is invisible in every other reading, so a
    /// test can hold it still: a world that opens with casting lamps costs ONE
    /// (the count and the clear strategy settle together) and none afterwards.
    /// Monotonic for the life of the engine; the monitor's `shadow.atlas` event
    /// is the same fact with a reason attached.
    unsigned atlasRebuilds = 0;
    /// ITEM VISITS MADE BY THE CASTER WALK, cumulative over every live scene
    /// (ENGINE-4 F5). The lamp-map cache has to notice a caster that moved,
    /// posed, changed shape or stopped casting, and it does it by walking the
    /// scene's items once a frame. That walk is O(items) and it used to run on
    /// EVERY frame of every drawn scene with a cacheable lamp — including
    /// frames in which nothing at all had happened. It is now gated on the same
    /// kind of epoch the GI movement scan uses (a transform write, or one of
    /// the pushed events that change a caster without moving it), so a STILL
    /// frame visits nothing: this number holding still across rendered frames
    /// IS the statement, and it is the one a test or the monitor can read.
    /// (It needs a host transform counter to be installed —
    /// Engine::setTransformWriteCounter — exactly like the GI half; without one
    /// the engine cannot know a host is not writing behind its back and the
    /// walk runs every frame, as it always did.)
    unsigned long long casterWalkItems = 0;
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
/// What the renderer's MEMORY POOLS hold (Engine::memoryStats, riders lane R4).
/// Two halves with two different owners:
///   * the GPU half is the VaoManager's buffer pools (every vertex/index/const
///     buffer and, on Vulkan, every texture — `gpuPoolsIncludeTextures`),
///     straight from VaoManager::getMemoryStats. `gpuPoolFreeBytes` is what is
///     ALLOCATED FROM THE DRIVER and not handed out; a pool that becomes
///     entirely free is returned to the driver by the Vulkan VaoManager on
///     its own, a few frames later (deallocateEmptyVbos from _update — this
///     pin's cleanupEmptyPools() THROWS ERR_NOT_IMPLEMENTED and is never
///     called by this engine). Watch capacity fall after a project closes.
///   * the SIMD half is the scene managers' SoA node/object pools, which grow
///     to the high-water mark of nodes ever alive and only shrink on
///     Engine::reclaimMemory (SceneManager::shrinkToFitMemoryPools). The pin
///     exposes no byte count for them (the per-depth ArrayMemoryManagers are
///     private), so the rows are the counts that drive them plus the
///     process's resident set, which is where a shrink shows.
struct MemoryStats {
    unsigned long long gpuPoolCapacityBytes = 0;  ///< bytes the VaoManager holds from the driver
    unsigned long long gpuPoolFreeBytes = 0;      ///< of which unused (fragmentation + empty pools)
    unsigned           gpuPools = 0;              ///< pool count
    bool               gpuPoolsIncludeTextures = false;
    unsigned           sceneManagers = 0;         ///< walked: every Scene + the document staging manager
    unsigned           simdNodes = 0;             ///< live scene nodes (both roots walked), summed — what the node pools hold
    unsigned           simdObjects = 0;           ///< live objects in the entity + light SoA pools, summed
    unsigned           simdNodeDepths = 0;        ///< node-hierarchy depth pools, summed (one SoA pool each)
    unsigned long long residentBytes = 0;         ///< the process RSS (Linux; 0 elsewhere)
};

/// ONE TEXTURE the renderer holds (app.textureMemory, lead 2026-09-09): the
/// attribution behind MemoryStats::gpuPoolCapacityBytes when textures share
/// the pools. `bytes` is the texture's own footprint (every mip, every
/// slice, MSAA counted); a pooled texture (`pooled`) lives in a slice of a
/// master array texture whose waste it does not carry. `residency` is the
/// engine's word: "OnStorage" (declared, nothing on the GPU), "OnSystemRam",
/// "Resident".
struct TextureMemoryEntry {
    std::string name;            ///< the alias the engine created it under
    std::string resource;        ///< the file or resource name behind it ("" for RTTs / manual)
    unsigned    width = 0, height = 0, depth = 0, slices = 0;
    unsigned    mipmaps = 0;
    unsigned    msaa = 1;
    std::string format;          ///< pixel format name
    unsigned long long bytes = 0;
    bool        renderTarget = false;
    bool        uav = false;
    bool        manual = false;  ///< uploaded by us (TextureFlags::ManualTexture)
    bool        pooled = false;  ///< AutomaticBatching: a slice of a pool master
    std::string residency;
};

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

/// GPU-DRIVEN COMPUTE DISPATCH, measured (ogre-patch 0032; suite
/// compute.indirect_dispatch). Photon's shared infrastructure, not a feature: a
/// compaction pass writes how many thread groups the next pass needs and the
/// next pass runs exactly that many, instead of being dispatched at its worst
/// case from the CPU. The pin had only vkCmdDispatch with CPU-side counts.
///
/// One call runs the whole two-job chain for one input size and reports what
/// came back, so a suite asserts numbers rather than trusting a log line.
struct IndirectDispatchProbe {
    /// The render system implements it at all (false on every backend but
    /// Vulkan, and on the NULL render system). Everything below is 0 then.
    bool supported = false;
    /// What the COUNTING job decided, read back from the argument buffer. Equals
    /// the number of non-zero entries in the input list it was given.
    unsigned groupsRequested = 0;
    /// How many groups the indirectly-dispatched job actually ran, counted from
    /// the output buffer (one stamped slot per group).
    unsigned groupsRan = 0;
    /// What those groups saw as gl_NumWorkGroups.x — the count the GPU read out
    /// of the buffer, which must equal groupsRequested. 0 when none ran.
    unsigned groupsSeen = 0;
    /// The same job dispatched the ordinary way, from a CPU-side count. The
    /// control: the two output buffers must be identical.
    unsigned groupsRanCpuSized = 0;
    /// True when the CPU-sized run and the indirect run produced byte-identical
    /// output buffers.
    bool matchesCpuSized = false;
    /// THE CONTROL FOR THE BARRIER. The same chain run once more with the
    /// compute-write -> indirect-read barrier suppressed
    /// (HlmsComputeJob::setIndirectDispatchBuffer's issueBarrier = false). A
    /// difference proves the barrier is load-bearing on this driver; agreement
    /// proves nothing either way (the hazard is real whether or not this GPU
    /// happens to lose the race), which is why the barrier is unconditional.
    unsigned groupsRanNoBarrier = 0;
    bool     noBarrierDiffered = false;
};

/// THE HIERARCHICAL DEPTH PYRAMID, as built (PostFxDesc::hzb; NANITE_SPEC
/// §4.3). Photon shared infrastructure: reported so a suite can assert the
/// shape instead of trusting it, and so a future consumer can ask whether there
/// is anything to read before it binds one.
struct HzbStatus {
    /// A pyramid exists in this view's compositor graph right now.
    bool built = false;
    /// Mip levels, i.e. compute passes: 1 (the seed) + one per reduction, down
    /// to 1x1. 11 at 1920x1080.
    unsigned levels = 0;
    /// Mip 0's size — the view's own, since the pyramid is full resolution.
    unsigned width = 0, height = 0;
    /// Which way is CLOSE (RenderSystem::isReverseDepth). True — the Vulkan
    /// default at this pin — means the near plane is 1 and a level holds the
    /// MAXIMUM of its footprint. Reported rather than assumed because the
    /// reduction operator flips with it.
    bool reverseDepth = true;
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

// ---------------------------------------------------------------------------
// THE RENDER-LOOP MONITOR (SPECS/RENDER_LOOP_MONITOR_SPEC.md)
// ---------------------------------------------------------------------------
// A DATA COLLECTOR, not a judge. The owner's words: "the purpose of the monitor
// is to collect as much data as possible for you to be able to review the core
// engine and how it's running, to look for issues and problems." It records
// what the frame did and WHY, and nothing in it computes a verdict: no budgets,
// no thresholds, no 'wasted' flags. Redundant work appears as work whose
// `reason` is `None`, which is data; whether that is a defect is the LEAD's
// reading, made from a capture, not the engine's.
//
// THREE RULES THIS BOUNDARY ENCODES:
//  1. OFF BY DEFAULT, ZERO COST WHEN OFF. At MonitorLevel::Off no listener is
//     attached to any workspace, no clock is read, no ring is allocated and no
//     GPU query pool exists (MonitorStatus reports all four, and a suite
//     asserts them).
//  2. FORWARD ONLY. Nothing is recorded until a capture starts; there is no
//     background history to look back at.
//  3. NO ON-SCREEN OUTPUT, EVER. The monitor writes records the host drains; it
//     never draws, because drawing would cost frame time and passes and
//     contaminate what it measures.

/// A FAULT INJECTED INTO ONE FRAME — TEST-FACING, NEVER A SHIPPING PATH
/// (`Engine::setFrameFault`, lane FRAME-CATCH-1, 2026-09-18).
///
/// The subject is the engine's own frame CLOSE: a frame that throws still owes
/// the OpenXR runtime an `xrEndFrame` (with the eye swapchain images it
/// acquired), owes the monitor a closed record, and must still reach the
/// device-lost latch. None of that can be asserted without a frame that
/// throws, and nothing a test can legally ask the engine to do throws out of a
/// frame on demand — so the engine can be asked for one.
///
///   * `Throw` — the next `frames` frames raise an Ogre exception from inside
///     the frame, AFTER the render has been recorded (and, in a session, after
///     the eye copies have acquired their swapchain images) and before
///     anything closes: the exact position a `VK_ERROR_DEVICE_LOST` from the
///     frame's commit occupies.
///   * `ThrowDeviceLost` — the same, and the frame ALSO reports the device as
///     lost to the engine's own latch. It is the only way `deviceLost()` can
///     become true without a real loss, and it fakes nothing else: the render
///     system, the driver and any live VR session are untouched (a real loss
///     cannot be induced on demand, and inducing one would take the box's GPU
///     down with it).
enum class FrameFault { None, Throw, ThrowDeviceLost };

/// What the monitor is doing. `Review` is the one recording level — the spec's
/// old Recorder/Compact/Full ladder collapsed to it when the HUD was cut.
enum class MonitorLevel {
    Off,     ///< nothing attached, nothing allocated, nothing read
    Review   ///< per-pass records, cache work + reasons, events, GPU samples
};

/// Which part of the frame a compositor pass belonged to. The classifier reads
/// the pass's parent NODE (the three shadow nodes are named constants) and the
/// workspace's owner, so it is exact rather than inferred from timings.
enum class PassBucket {
    Other,          ///< anything unclassified — a warm-up workspace, a mipmap chain
    Main,           ///< the view's own camera scene pass
    Post,           ///< the view's post chain (quads, computes, resolves)
    ShadowView,     ///< the view's shadow node (kShadowNodeName)
    ShadowReflect,  ///< a planar mirror's shadow node (kReflectShadowNodeName)
    ShadowProbe,    ///< a reflection probe's shadow node (kProbeShadowNodeName)
    Planar,         ///< a planar-mirror reflection render
    ProbeFace       ///< a reflection-probe cube face
};

/// One compositor pass, as executed. `draws`/`batches`/`triangles`/`instances`
/// are the DELTA of the render system's own metrics across the pass, so summing
/// a frame's passes reproduces `RenderStats::draws` for that frame exactly —
/// which is what the suite asserts.
///
/// ZERO DRAWS DOES NOT ALWAYS MEAN "NOTHING WAS DRAWN", and this is Ogre's
/// plumbing rather than the monitor's. MEASURED (lane MON-P1a, 2026-09-12, the
/// engine suite's offscreen rig): a frame whose view renders a ground, a cube
/// and the overlays reports the view's own scene pass as `draws = 0`, while the
/// shadow node's cube-face caster passes in the SAME frame report 1 each — so
/// the frame's total is 3 on a frame that re-rendered a lamp map and 0 on the
/// idle frames after it, with an identical, complete picture every time. In the
/// app the main pass does report draws (`scripting.e2e.render_stats` asserts
/// `draws > 0`), so the under-count is configuration-dependent and was NOT
/// root-caused here; it is reported upstream-ward rather than guessed at.
/// The monitor reports the render system's own numbers faithfully; analysis
/// must therefore read a zero as "the renderer counted nothing here", never as
/// "this pass drew nothing". `FrameRecord::metricsRecording` says whether the
/// counters were live at all.
struct FramePass {
    std::string workspace;        ///< the workspace instance's definition name
    std::string node;             ///< the parent compositor node's name
    std::string pass;             ///< the definition's profiling id, else its type
    PassBucket  bucket = PassBucket::Other;
    /// The shadow map index a shadow pass renders; kNoShadowMap otherwise.
    unsigned    shadowMapIdx = 0xFFFFFFFFu;
    static constexpr unsigned kNoShadowMap = 0xFFFFFFFFu;
    unsigned    draws = 0, batches = 0, instances = 0;
    unsigned long long triangles = 0;
    /// TRUE when the pass never reported its end (a workspace update closed
    /// with it still open). Its times and counts are unknown, not zero:
    /// `cpuMs` is negative. Seeing one of these is itself the finding.
    bool        orphaned = false;
    float       cpuMs = 0.0f;
    /// SCENE PASSES ONLY: of this pass's wall time, how much went on its shadow
    /// node's update (its own nested pass records included) — the shadow-vs-
    /// scene split, taken from `passSceneAfterShadowMaps`. NEGATIVE on every
    /// pass that is not a scene pass (quads, clears, computes, resolves). ZERO
    /// on a scene pass whose shadow node executed nothing — which is exactly
    /// what a fully cached lamp set looks like, and is why the zero matters as
    /// much as the number. Ogre fires the callback on every scene pass, a
    /// shadow node's own caster passes included, so those carry it too.
    float       shadowMs = -1.0f;
    /// GPU milliseconds from timestamp queries. NEGATIVE means NOT MEASURED —
    /// the build has no JAH_GPU_TIMESTAMPS, the device has no usable
    /// timestamps, or the result has not come back yet. Never faked as 0.
    float       gpuMs = -1.0f;
};

/// The caches whose work and reason the monitor records (§4.7).
enum class CacheKind {
    Probe,      ///< a reflection-probe capture (units = cube faces)
    ShadowMap,  ///< a shadow map render (units = passes; id = the light's NodeId)
    Gi,         ///< the GI volume: voxelize / IR trace / IFD converge
    Planar,     ///< a planar reflector's render (view-dependent: always justified)
    Shader,     ///< a shader/PSO compile (detail = the permutation)
    Texture     ///< a texture load (units = bytes/1024, detail = the name)
};

/// WHY a cache redid its work. `None` is the important value: the cache did the
/// work with no recorded input change. That is recorded as data and judged by
/// nobody here.
///
/// The names come from the invalidation causes that already exist in the
/// engine — `GiStaleReason` (including `Mobility`, lane R1), the lamp-map
/// cache's per-light dirty keys (lane E2) and the probe sweep's own bookkeeping
/// — so a reason is a REPORT of what the engine decided, never a second guess.
enum class WorkReason {
    None,        ///< no recorded input change (the redundant-work value)
    Build,       ///< first build / the arm was created this frame
    Rebuild,     ///< a full teardown-and-rebuild
    Refresh,     ///< an explicit refresh request (refreshGlobalIllumination)
    Sweep,       ///< the round-robin budget's turn came up (no input changed)
    Moved,       ///< the owner itself moved
    Caster,      ///< a caster moved or changed inside its range/volume
    Light,       ///< a light moved or changed
    Material,    ///< a material or texture changed
    Sky,         ///< the sky changed
    Ambient,     ///< the ambient term changed
    Fog,         ///< fog changed
    Mobility,    ///< an object's mobility class changed (R1)
    Added,       ///< an object was added
    Removed,     ///< an object was removed
    Bounds,      ///< the volume/region bounds changed
    Resolution,  ///< resolution, quality or budget changed (an atlas rebuild)
    Camera,      ///< the camera moved (planar reflections; a view-dependent cache)
    Permutation, ///< a shader permutation was seen for the first time
    Request      ///< the host asked for it directly
};

/// One cache's work in one frame, with its reason.
struct CacheWork {
    CacheKind  cache  = CacheKind::Probe;
    WorkReason reason = WorkReason::None;
    /// Probe index, light NodeId, planar slot — 0 when the cache is global.
    unsigned long long id = 0;
    /// Free text the analysis reads: the permutation, the texture name, the
    /// shadow node kind, the GI phase.
    std::string detail;
    /// Faces captured, maps rendered, compiles, kilobytes — per `cache`.
    unsigned   units = 0;
    /// Milliseconds, or NEGATIVE when this work was not timed separately
    /// (it is inside the pass records instead).
    float      ms = -1.0f;
    /// GPU milliseconds for this work's OWN dispatches, from the render
    /// system's timestamp queries (ogre-patch 0027). NEGATIVE means NOT
    /// MEASURED — the build has no JAH_GPU_TIMESTAMPS, the device has no
    /// usable timestamps, the work ran outside a recorded frame, or the result
    /// has not come back yet. Never faked as 0.
    ///
    /// It is the ONLY GPU time in a capture that does not come from a
    /// compositor pass: a GI voxelisation, a light injection and an irradiance
    /// field's integration are compute dispatches the compositor never sees,
    /// so they are absent from `FrameRecord::gpuMs` (which stays the sum of the
    /// frame's passes — the invariant a suite asserts) and appear only here.
    float      gpuMs = -1.0f;
};

/// One stage of the frame, exclusive of its children.
struct FrameStage {
    std::string name;          ///< "engine.pre", "engine.record", "engine.swap", host stages
    float       ms = 0.0f;
};

/// Why this frame was rendered. Set by the caller through
/// `Engine::setNextFrameCause` — the driver tick, a script's `editor.frame`, an
/// offscreen readback, the warm-up gate — so analysis can tell a frame nobody
/// saw from one the owner watched.
enum class FrameCause { Unknown, Driver, Scripted, Offscreen, WarmUp, Player };

/// EVERYTHING ONE `renderOneFrame` DID. One of these per frame while a capture
/// runs; the host drains them with `takeFrameRecords` and writes them to
/// `frames.jsonl`.
struct FrameRecord {
    unsigned long long frame = 0;    ///< the engine's monotonic frame counter
    /// Milliseconds since the capture started, at the frame's first stage mark.
    double      startMs = 0.0;
    float       totalMs = 0.0f;      ///< the whole renderOneFrame, wall clock
    FrameCause  cause = FrameCause::Unknown;
    bool        onscreen = false;    ///< an enabled WINDOW view took part
    unsigned    scenesUpdated = 0;
    std::vector<FrameStage> stages;
    std::vector<FramePass>  passes;
    std::vector<CacheWork>  cacheWork;
    // ---- counters (all for THIS frame) ----
    unsigned    draws = 0, batches = 0, instances = 0;
    unsigned long long triangles = 0;
    unsigned    probeCaptures = 0;      ///< probe cube faces captured
    /// Photon cascade re-voxelisations in THIS frame. The scheduler's contract
    /// is "at most one per frame", and this is what makes that assertable from
    /// a bundle without parsing the `vct.cascadeN` rows out of cacheWork.
    unsigned    cascadeRebuilds = 0;
    unsigned    shadowPasses = 0;       ///< the view's shadow node
    unsigned    shadowPassesReflect = 0;///< planar mirrors' shadow nodes
    unsigned    shadowPassesProbe = 0;  ///< probes' shadow nodes
    unsigned    planarRenders = 0;
    unsigned    shaderCompiles = 0;
    /// Was the render system COUNTING while this frame rendered? Recording is
    /// off in Ogre until something asks for `renderStats()`, and a frame
    /// rendered with it off reports zeros for every geometry counter — a fact
    /// `frames.jsonl` states outright so analysis never has to infer it.
    bool        metricsRecording = false;
    /// Passes that were closed by a workspace boundary rather than by their own
    /// `passPosExecute` (see `FramePass::orphaned`). Zero is the only value
    /// seen so far; a non-zero one means this frame's pass tree is incomplete.
    unsigned    orphanedPasses = 0;
    float       textureWaitMs = 0.0f;   ///< the frame-head streaming drain
    /// Σ of the passes' GPU milliseconds, or NEGATIVE when unmeasured.
    float       gpuMs = -1.0f;
    /// What the monitor itself cost this frame, so analysis can subtract it.
    float       overheadMs = 0.0f;
};

/// A discrete thing that happened, with its cause.
enum class MonitorEventKind {
    GiRebuild,        ///< a GI arm was torn down and rebuilt
    GiRefresh,        ///< every probe / the voxel volume was invalidated
    ProbeGridBuild,   ///< the PCC probe grid was (re)placed
    AtlasRebuild,     ///< the shadow atlas changed shape (workspaces recreated)
    WorkspaceRebuild, ///< a view's workspace was recreated
    ShaderCompile,    ///< a permutation was compiled (label = the permutation)
    TextureLoad,      ///< a texture finished loading (value = bytes)
    VramFlush,        ///< a deferred-free flush / device memory was reclaimed
    DeviceLost,       ///< the device was lost
    ViewDestroyed,
    Host              ///< the host's own hook: page switches, UI gaps, script marks
};

/// One event. Times share the frame records' clock.
struct MonitorEvent {
    MonitorEventKind kind = MonitorEventKind::Host;
    unsigned long long frame = 0;
    double      startMs = 0.0;
    float       ms = -1.0f;          ///< duration, negative when instantaneous
    WorkReason  reason = WorkReason::None;
    std::string label;               ///< the host's label, a permutation, a path
    std::string detail;
    unsigned long long value = 0;    ///< bytes, counts — per `kind`
};

/// What the monitor is doing right now — and the four assertions that make
/// "zero cost when off" checkable from a test rather than believed.
struct MonitorStatus {
    MonitorLevel level = MonitorLevel::Off;
    /// Monitor listeners attached across EVERY live workspace (view, planar,
    /// probe). MUST be 0 at Off.
    unsigned attachedListeners = 0;
    unsigned ringCapacity = 0;       ///< 0 at Off: the ring is freed, not kept
    unsigned ringFrames = 0;         ///< records waiting to be drained
    unsigned pendingEvents = 0;
    unsigned long long framesRecorded = 0;
    /// Records the ring overwrote because nobody drained fast enough. Non-zero
    /// is a fact about the host's drain rate, not a failure.
    unsigned long long framesDropped = 0;
    /// EVENTS the event list could not hold (its capacity is fixed and it drops
    /// rather than grow). On the boundary for the same reason `framesDropped`
    /// is: a capture bundle certifies itself complete or incomplete, and it
    /// cannot do that honestly if the host cannot see what the engine dropped
    /// (lane MON-P1b review, 2026-09-13 — a bundle claimed completeness while
    /// events had been dropped past the cap).
    unsigned long long eventsDropped = 0;
    // ---- GPU timing (P1c), with BOTH its off-switches visible ----
    bool     gpuCompiled  = false;   ///< the engine was built with JAH_GPU_TIMESTAMPS
    bool     gpuSupported = false;   ///< ...and the device/backend can do timestamps
    bool     gpuActive    = false;   ///< ...and a capture has a query pool open NOW
    unsigned gpuQueryPools = 0;      ///< MUST be 0 outside a capture
    /// GPU samples the LAST frame could not record because the query pool ran
    /// out of room. Non-zero means this capture's GPU numbers are INCOMPLETE —
    /// said out loud rather than left for analysis to notice that some passes
    /// have no time. (A probe capture alone is 6 faces x ~22 passes.)
    unsigned gpuSamplesTruncated = 0;
    std::string gpuReason;           ///< why GPU timing is unavailable, when it is
    float    overheadMs = 0.0f;      ///< the monitor's own cost, last frame
};

// ---- Engine snapshot (§4.8 `snapshot_start.json` / `snapshot_end.json`) ----

/// One pass of the compositor graph, as DEFINED (not as executed).
struct CompositorPassInfo {
    std::string type;            ///< "render_scene", "render_quad", "clear", "compute", ...
    std::string profilingId;
    std::string camera;          ///< the camera the definition names, when it names one
    /// The shadow node a SCENE pass names, empty when it names none. The single
    /// most expensive line in a capture's graph: a pass that names one
    /// re-renders it, and a probe face that names the VIEW's node costs a
    /// full-resolution atlas per probe.
    std::string shadowNode;
    unsigned    shadowMapIdx = FramePass::kNoShadowMap;
    unsigned    numInitialPasses = 0;   ///< 0 = every frame; >0 = only the first N
};

struct CompositorNodeInfo {
    std::string name;
    std::vector<CompositorPassInfo> passes;
};

/// One live workspace. THE COMPOSITOR GRAPH the spec asks for is the list of
/// these: every workspace, its nodes and passes, and which scene each renders.
struct CompositorWorkspaceInfo {
    std::string name;            ///< the workspace definition's name
    std::string owner;           ///< "view:main", "probe:7", "planar:0", "?"
    std::string scene;           ///< the SceneManager it renders
    bool        enabled = false;
    unsigned    width = 0, height = 0;
    unsigned    listeners = 0;   ///< listeners attached to it (the monitor's included)
    std::vector<CompositorNodeInfo> nodes;
};

/// One reflection probe, as placed.
struct ProbeInfo {
    unsigned index = 0;
    Vec3     centre, halfSize;
    /// The probe's parallax SHAPE (what the shader reprojects onto).
    Vec3     shapeMin, shapeMax;
    bool     dirty = false;      ///< scheduled to re-capture
    bool     isStatic = false;   ///< placed as a static (never-swept) probe
    unsigned resolution = 0;
};

/// One light, as the engine holds it — with whether the lamp-map cache has it.
struct SnapshotLight {
    NodeId      node = 0;
    LightType   type = LightType::Point;
    bool        castShadow = false;
    bool        cached = false;     ///< the lamp-map cache holds its map
    bool        dirty = false;      ///< ...and it re-renders on the next frame
    unsigned    shadowSlot = 0xFFFFFFFFu;   ///< the atlas slot, or ~0
    float       range = 0.0f;
    float       intensity = 0.0f;
    Vec3        position;
};

/// THE WHOLE ENGINE, at one instant. Written at the start and the end of a
/// capture so analysis can see what changed under it. Built from the status
/// readers that already exist plus the compositor graph, which nothing else
/// could see.
struct EngineSnapshot {
    bool        live = false;
    std::string label;              ///< "start" / "end" / whatever the host passes
    std::string scene;              ///< the scene the snapshot describes
    unsigned long long frame = 0;
    double      atMs = 0.0;         ///< capture-relative milliseconds
    DeviceInfo  device;
    GiParams    giParams;           ///< every GI parameter, as requested
    GiStatus    gi;                 ///< ...and what they resolved to
    ShadowStatus shadow;
    ShaderCacheStats shaderCache;
    ObjectCounts objects;
    MemoryStats  memory;
    EngineThreading threading;
    MobilityStatus  mobility;
    RenderStats  render;
    /// The texture streaming queue at this instant.
    bool        texturesDoneStreaming = true;
    unsigned    texturesPending = 0;
    /// Datablocks held per Hlms block (pbs, unlit, low-level...). Ogre keeps
    /// the compiled-shader cache itself private at this pin, so this is the
    /// exact number the snapshot can state — and it is the one behind the
    /// batching-collapse question.
    std::vector<std::pair<std::string, unsigned>> hlmsDatablocks;
    /// VRAM by pool — the texture-manager entries, LARGEST FIRST and capped
    /// (a bundle must not be dominated by this list). `textureCount` is how
    /// many there really are and `texturesTruncated` how many were dropped, so
    /// a reader is never silently given a partial list.
    std::vector<TextureMemoryEntry> textures;
    unsigned textureCount = 0;
    unsigned texturesTruncated = 0;
    std::vector<SnapshotLight> lights;
    std::vector<ProbeInfo>     probes;
    std::vector<CompositorWorkspaceInfo> workspaces;
};

}}  // namespace jahshaka::engine
