#pragma once
// THE ENGINE's MESH TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

/// CPU-side triangle mesh, the shape the assimp importer produces at import time.
/// positions: xyz per vertex (required). normals: xyz per vertex (optional — smooth
/// normals are generated when empty). uvs: uv per vertex (optional). indices: three
/// per triangle (required).
// ---- ATOM stage 1: THE LEVEL THAT STANDS IN FOR A MESH AT A GIVEN SIZE -----
//
// THE RULE, written ONCE and cited from all of its callers
// (`MeshData::lodForWorldError` below; the device's `jahLevelForAllowed` in
// irisgl/engine/media/Hlms/Jahshaka/JahLevelRule_piece_cs.any, which the voxel
// gather (JahVoxelGather_cs.glsl) spends on
// Photon's cascades on the device (terms from OgreScene::cascadeGatherInputs); and the
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
// `bounds[i]` is level i+1's MEASURED TWO-SIDED DISTANCE from level 0, as a
// LENGTH in the same units as `cellSize` (see MeshData::lodBounds), and the
// bounds are non-decreasing, so the first level that fails the test ends the
// walk. The answer is 0 — the authored geometry — whenever even level 1 is too
// coarse, and whenever the size is not a positive finite number.
//
// WHY "BELOW THE ALLOWED DEVIATION" IS THE WHOLE RULE: the baked bound is how
// far a level's surface is measured to sit from the authored one, so a consumer
// that cannot see a difference of `allowed` cannot see that level either. What
// `allowed` IS belongs to the consumer and to nobody else — and the shipped
// consumers have measured it, which is the only way this number was ever going
// to be right (the old text claimed "a sample of its own resolution" for the
// voxeliser and was wrong by two orders of magnitude; see OgreGi.cpp).
//
// THE ARRAY IS `lodBounds` AND NOT `lodErrors` SINCE ATOM-BAKE-1 (ATOM P1's
// AT-A5), and the difference is the whole point of that lane. What used to be
// passed here was meshoptimizer's `result_error` — a running max over
// area-weighted MEAN quadric errors, mixed with attribute deviation, documented
// by its own author as an approximation — under a comment in this file promising
// it was "the worst distance a level's surface may sit from the authored one".
// It was not, and four consumers leaned on the promise. What is passed now is a
// sampled Hausdorff distance MEASURED at bake time between the two surfaces,
// with the sampling-gap margin applied (irisgl/import/meshbake.cpp, the block
// headed THE MEASURED BOUND). `lodErrors` still exists and no consumer reads it.
//
// `levelsAvailable` caps the answer at the levels the consumer actually has —
// the document's index lists, or the VAOs the engine built from them.
//
// ---- THE QUALITY CURRENCY (ATOM P3's SUB-ERROR) ---------------------------
//
// THE THREE CONSUMERS SPOKE THREE DIALECTS OF ONE SENTENCE. The view said
// "pixels at proj[1][1] and a viewport height", the cascade said "1/256 of a
// cell", the surface card said "a card texel". They are the same statement:
//
//     a level stands in for the authored mesh when its measured deviation is
//     smaller than the SAMPLE the consumer takes.
//
// So there is ONE derivation and it has two halves, `sampleFootprint*` (how big
// one sample is, in world metres, where the object is) and `allowedWorldError`
// (how many of those samples of deviation this consumer tolerates). Everything
// else — the cascade's 1/256, the card's one texel, the view's one pixel — is a
// TOLERANCE in sample units, which is the number worth arguing about, and the
// arithmetic around it is shared. `pixelError` is the same relation read the
// other way (a deviation expressed in samples), and it is what a cull's shader
// computes because the GPU has the bound and wants the count.
//
// THE GLSL TWIN of the three functions below is ONE piece file,
// media/Hlms/Jahshaka/JahLevelRule_piece_cs.any (`jahSampleFootprint`,
// `jahAllowedWorldError`, `jahLevelForAllowed`), inserted by the cull's job 1 and
// the voxel gather, and `engine.lod_rule_parity` asserts the two halves agree
// through both jobs on the device. There are two halves because a compute shader
// cannot include a C++ header; there is one RULE because the suite fails when they drift.

/// THE WORLD SIZE OF ONE SAMPLE of a PERSPECTIVE view, at an object whose
/// nearest surface is `distanceToSurface` metres away (Ogre's own
/// `distance(worldAabbCentre, eye) - worldRadius`, clamped at 0).
/// `projScaleY` is proj[1][1]; `viewportHeight` is the pass's own target height.
/// This is clusterlod.h's projection inverted, and it is what `worldPerPixel`
/// in OgreMesh.cpp's LOD strategy multiplies by.
inline float sampleFootprintPerspective(float distanceToSurface, float projScaleY,
                                        float viewportHeight) {
    if (!(projScaleY > 0.0f) || !(viewportHeight > 0.0f)) return 0.0f;
    return distanceToSurface * 2.0f / (projScaleY * viewportHeight);
}

/// The same for an ORTHOGRAPHIC view: one sample is the ortho window's height
/// divided by the target's, and it does not depend on distance.
inline float sampleFootprintOrtho(float orthoWindowHeight, float viewportHeight) {
    if (!(viewportHeight > 0.0f)) return 0.0f;
    return orthoWindowHeight / viewportHeight;
}

/// WHAT A CONSUMER MAY AFFORD, in the units the BAKED BOUNDS are measured in.
///
/// `tolerance` is in SAMPLES (the view's pixel budget, the cascade's fraction of
/// a cell, the card's texel count); `footprint` is the world size of one sample
/// at the object; `meshToWorldScale` is the largest axis scale of the instance's
/// transform, because `MeshData::lodBounds` are measured in the MESH's own units
/// and a 10x-scaled instance shows ten times that deviation in the world.
inline float allowedWorldError(float tolerance, float footprint, float meshToWorldScale = 1.0f) {
    if (!(tolerance > 0.0f) || !(footprint > 0.0f)) return 0.0f;
    if (!(meshToWorldScale > 0.0f)) return 0.0f;
    return tolerance * footprint / meshToWorldScale;
}

/// `meshToWorldScale` FROM A TRANSFORM: the largest axis scale of a ROW-MAJOR 3x4
/// world matrix (twelve floats, the GPU scene's and VkTransformMatrixKHR's layout)
/// is its longest COLUMN. M = T R S, so column j is R times axis j times s_j and its
/// length is exactly s_j; a ROW mixes the scales through the rotation and, under a
/// rotation times a non-uniform scale, is shorter than the largest s_j - which
/// divides the allowance by too small a scale and picks a coarser level than the
/// tolerance permits. The shaders' twin is `jahWorldMaxAxisScale`
/// (JahLevelRule_piece_cs.any); engine.lod_rule_parity holds the two together.
inline float worldMaxAxisScale(const float *rowMajor3x4) {
    float s = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float x = rowMajor3x4[c], y = rowMajor3x4[4 + c], z = rowMajor3x4[8 + c];
        s = std::max(s, std::sqrt(x * x + y * y + z * z));
    }
    return s;
}

/// THE SAME RELATION READ BACKWARDS: how many samples of deviation a bound of
/// `bound` mesh-units shows at this footprint. A cull's shader computes this and
/// compares it against the tolerance; a report prints it. 0 footprint (a pass
/// with no measurable projection) answers 0, which reads as "no error", and the
/// level walk's `allowed <= 0` guard is what keeps that case on level 0.
inline float pixelError(float bound, float footprint, float meshToWorldScale = 1.0f) {
    if (!(footprint > 0.0f)) return 0.0f;
    return bound * meshToWorldScale / footprint;
}

inline size_t lodLevelForWorldError(const std::vector<float> &bounds, float allowed,
                                   size_t levelsAvailable) {
    if (!(allowed > 0.0f)) return 0;
    size_t level = 0;
    for (size_t i = 0; i < bounds.size() && i < levelsAvailable; ++i) {
        if (!(bounds[i] < allowed)) break;    // bounds are non-decreasing
        level = i + 1;
    }
    return level;
}

// ---- ATOM stage 2: THE CLUSTER CUT (lane ATOM-CLUSTER-1) -------------------
//
// SPECS/atom/B2_CLUSTER_DAG_DESIGN.md §2. The mesh's CLUSTER DAG is a bake
// product (irisgl/import/meshbake.cpp `clusterdag`, document/assets/mesh.h
// `iris::MeshClusterDag`) handed across as the two tables below plus one global
// index stream (`MeshData::clusterIndices`). THE PRODUCT DRAWS IT (ATOM-CLUSTER-CUT,
// SPECS/v2/CLUSTER_CUT_DESIGN.md): the GPU cull evaluates this rule per (instance,
// cluster) and the id pass draws the cut; the test harness (tests/atom/cluster_draw.*)
// is the rule's CPU-side proof.
//
// THE RULE IS clusterlod.h's RENDER TEST (its lines 129-133), with the MEASURED
// group error in the quality currency's units:
//
//     a cluster is DRAWN iff its own group is NOT affordable
//                        and (it is level 0, or its `refined` group IS affordable)
//
// and a group is AFFORDABLE when its measured error is STRICTLY below what the
// consumer can afford at that group — `allowedWorldError` at the group's own
// distance, exactly the level walk's comparison (`lodLevelForWorldError`), so a
// mesh whose DAG and chain agree on an error agree on the answer. The distance
// is the currency's: from the eye to the group's SPHERE (centre transformed by
// the instance, radius times the instance's largest axis scale), clamped at 0.
//
// WHY IT IS ONE CUT: the bake makes every group's error at least every child
// group's, and every group's sphere contain every child group's — so a group
// that is affordable has only affordable children, and the drawn clusters are
// exactly one frontier through the DAG. `atom.cut_partition` asserts it of
// `clusterCut` over the harness meshes (no drawn cluster under a drawn one,
// every level-0 triangle covered once); `engine.lod_rule_parity` holds the
// device's cut equal to `clusterCut`'s.
//
// THE GLSL TWIN is media/Hlms/Jahshaka/JahClusterCut.glsl (`jahClusterGroupAllowed`,
// `jahClusterDrawn`); `engine.lod_rule_parity`'s fourth copy runs it on the device
// over the harness meshes' DAGs and compares the drawn set with `clusterCut`'s.
// A change here that is not made there fails that suite.

/// One cluster of a mesh's DAG. `firstIndex`/`indexCount` are the cluster's
/// contiguous range of `MeshData::clusterIndices` (and of the engine's uploaded
/// cluster stream); the sphere is the header's culling bound, mesh space.
struct MeshCluster {
    unsigned firstIndex = 0;
    unsigned indexCount = 0;
    int      group = -1;       ///< the group this cluster is a MEMBER of
    int      refined = -1;     ///< the group whose simplification PRODUCED it; -1 = level 0
    float    centre[3] = { 0.0f, 0.0f, 0.0f };
    float    radius = 0.0f;
};

/// One group of a mesh's DAG. `error` is the MEASURED two-sided distance of the
/// group's simplified geometry from the level-0 surface it stands for — mesh
/// units, monotone up the DAG, `FLT_MAX` for a terminal group (never affordable).
/// `estimate` is clusterlod.h's own number, a diagnostic nothing selects on.
struct MeshClusterGroup {
    int   depth = 0;
    float centre[3] = { 0.0f, 0.0f, 0.0f };
    float radius = 0.0f;
    float error = 0.0f;
    float estimate = 0.0f;
};

/// ONE INSTANCE AS ONE VIEW SEES IT — everything the cut reads that is not the
/// DAG. `worldRow` is the instance's 3x4 transform as three ROWS (the GPU scene
/// table's own layout); `scale` is its largest axis scale, the currency's
/// `meshToWorldScale`, supplied by the caller exactly as the cull derives it.
struct ClusterCutView {
    float worldRow[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f },
                             { 0.0f, 1.0f, 0.0f, 0.0f },
                             { 0.0f, 0.0f, 1.0f, 0.0f } };
    float scale = 1.0f;
    float eye[3] = { 0.0f, 0.0f, 0.0f };
    float tolerance = 0.0f;       ///< samples (the view's pixel budget)
    float projScaleY = 0.0f;      ///< proj[1][1]
    float viewportHeight = 0.0f;  ///< the pass's target height
    bool  orthographic = false;   ///< no distance term: one metre (the level walk's ortho case)
    /// THE REQUEST'S SIX PLANES (SPEED-VR-MEM, the cut's per-cluster frustum test,
    /// JahCullCut_cs.glsl `jahClusterInFrustum`): inward, normalised (a, b, c, d), world
    /// space — the cull request's own. `cullPlanes` false = no frustum term (a DAG-only
    /// question: the threshold sweeps, the crack proofs).
    bool  cullPlanes = false;
    float planes[6][4] = {};
};

/// THE CLUSTER'S FRUSTUM TEST, spelled operation for operation the way the GLSL twin
/// spells it: the cluster's mesh-space sphere carried to world space by the instance's
/// rows (the centre transformed, the radius by the largest axis scale), a relative margin
/// of 2^-12 plus 1 micrometre, and dropped only when wholly behind one of the four SIDE
/// planes (0-3; never the depth pair - a shadow camera's depth clamp draws a caster in
/// front of its near plane).
inline bool clusterInFrustum(const MeshCluster &c, const ClusterCutView &v) {
    if (!v.cullPlanes) return true;
    float w[3];
    for (int r = 0; r < 3; ++r)
        w[r] = v.worldRow[r][0] * c.centre[0] + v.worldRow[r][1] * c.centre[1] +
               v.worldRow[r][2] * c.centre[2] + v.worldRow[r][3];
    float rad = c.radius * v.scale;
    rad += rad * (1.0f / 4096.0f) + 1.0e-6f;
    for (int i = 0; i < 4; ++i)   // the four SIDE planes: a shadow camera draws with depth clamp
        if (v.planes[i][0] * w[0] + v.planes[i][1] * w[1] + v.planes[i][2] * w[2] + v.planes[i][3] < -rad)
            return false;
    return true;
}

/// ONE SAMPLE'S WORLD SIZE AT ONE GROUP (the currency's footprint at the group's own
/// distance). The arithmetic is spelled operation for operation the way the GLSL twin
/// spells it (JahClusterCut.glsl jahClusterGroupDistance + jahSampleFootprint).
inline float clusterGroupFootprint(const MeshClusterGroup &g, const ClusterCutView &v) {
    const float cx = v.worldRow[0][0] * g.centre[0] + v.worldRow[0][1] * g.centre[1] +
                     v.worldRow[0][2] * g.centre[2] + v.worldRow[0][3];
    const float cy = v.worldRow[1][0] * g.centre[0] + v.worldRow[1][1] * g.centre[1] +
                     v.worldRow[1][2] * g.centre[2] + v.worldRow[1][3];
    const float cz = v.worldRow[2][0] * g.centre[0] + v.worldRow[2][1] * g.centre[1] +
                     v.worldRow[2][2] * g.centre[2] + v.worldRow[2][3];
    const float dx = cx - v.eye[0], dy = cy - v.eye[1], dz = cz - v.eye[2];
    const float d = v.orthographic ? 1.0f
                                   : std::max(0.0f, std::sqrt(dx * dx + dy * dy + dz * dz) - g.radius * v.scale);
    return sampleFootprintPerspective(d, v.projScaleY, v.viewportHeight);
}

/// What the consumer can afford AT ONE GROUP, in mesh units (jahClusterGroupAllowed).
inline float clusterGroupAllowed(const MeshClusterGroup &g, const ClusterCutView &v) {
    return allowedWorldError(v.tolerance, clusterGroupFootprint(g, v), v.scale);
}

/// THE COMPARISON, once: the level walk's strictness (a bound EQUAL to what is
/// afforded is not taken), so `allowed <= 0` affords nothing and the cut is level 0.
inline bool clusterGroupAffordable(float error, float allowed) {
    return allowed > 0.0f && error < allowed;
}

/// ONE GROUP'S VERDICT IN ONE VIEW — the rule the cut job evaluates per group (the GLSL twin
/// jahClusterGroupVerdict): THE EXACT RULE, error < allowed. The owner's law (ATOM-6): the switch
/// error stays under one pixel at every tier; triangles are earned by better levels, never by a
/// wider threshold.
inline bool clusterGroupVerdict(const MeshClusterGroup &g, const ClusterCutView &v) {
    return clusterGroupAffordable(g.error, allowedWorldError(v.tolerance, clusterGroupFootprint(g, v), v.scale));
}

/// THE RENDER TEST over per-group answers (`affordable[g]` for every group).
inline bool clusterDrawn(const MeshCluster &c, const std::vector<unsigned char> &affordable) {
    if (affordable[size_t(c.group)]) return false;
    return c.refined < 0 || affordable[size_t(c.refined)] != 0;
}

/// THE ROOT CUT (ATOM-7 stage 4; the GLSL twin JahClusterCut.glsl jahClusterRootDrawn, which the cull's
/// coarse reserve draws on a stream overflow): the cut at an infinite tolerance, every finite group
/// affordable. A cluster is drawn when its own group is the LAST of its chain — terminal, or finite and
/// producing nothing (the empty top the prune leaves: before, the reserve drew nothing for such a chain) —
/// and it is a leaf or was produced by a finite group. `produced[g]` = the clusters group g produced
/// (`clusterProducedCounts`; the upload's error[3]). The threshold is the GLSL's literal.
constexpr float kClusterTerminal = 3.0e38f;
inline std::vector<unsigned> clusterProducedCounts(const std::vector<MeshCluster> &clusters, size_t groupCount) {
    std::vector<unsigned> produced(groupCount, 0u);
    for (const MeshCluster &c : clusters)
        if (c.refined >= 0 && size_t(c.refined) < groupCount) ++produced[size_t(c.refined)];
    return produced;
}
inline bool clusterRootDrawn(const MeshCluster &c, const std::vector<MeshClusterGroup> &groups,
                             const std::vector<unsigned> &produced) {
    const bool last = groups[size_t(c.group)].error >= kClusterTerminal || produced[size_t(c.group)] == 0u;
    if (!last) return false;
    return c.refined < 0 || groups[size_t(c.refined)].error < kClusterTerminal;
}

/// The cut from per-group answers: the drawn clusters' indices, in cluster
/// order, into `out` (cleared). Returns the number of TRIANGLES drawn.
inline size_t clusterCutFromAffordable(const std::vector<MeshCluster> &clusters,
                                       const std::vector<unsigned char> &affordable,
                                       std::vector<unsigned> &out) {
    out.clear();
    size_t triangles = 0;
    for (size_t i = 0; i < clusters.size(); ++i)
        if (clusterDrawn(clusters[i], affordable)) {
            out.push_back(unsigned(i));
            triangles += clusters[i].indexCount / 3u;
        }
    return triangles;
}

/// THE CUT FOR ONE INSTANCE IN ONE VIEW — each group's own distance.
inline size_t clusterCut(const std::vector<MeshClusterGroup> &groups,
                         const std::vector<MeshCluster> &clusters, const ClusterCutView &view,
                         std::vector<unsigned> &out) {
    std::vector<unsigned char> affordable(groups.size(), 0);
    for (size_t g = 0; g < groups.size(); ++g)
        affordable[g] = clusterGroupVerdict(groups[g], view) ? 1 : 0;
    const size_t triangles = clusterCutFromAffordable(clusters, affordable, out);
    if (!view.cullPlanes) return triangles;
    // ...and the view's frustum, cluster by cluster (the cut job's own test).
    size_t kept = 0, keptTris = 0;
    for (unsigned c : out)
        if (clusterInFrustum(clusters[c], view)) {
            out[kept++] = c;
            keptTris += clusters[c].indexCount / 3u;
        }
    out.resize(kept);
    return keptTris;
}

/// THE CUT AT ONE ALLOWED ERROR FOR EVERY GROUP (a threshold sweep, and the
/// comparison with the chain at the same `allowed`).
inline size_t clusterCutAtAllowed(const std::vector<MeshClusterGroup> &groups,
                                  const std::vector<MeshCluster> &clusters, float allowed,
                                  std::vector<unsigned> &out) {
    std::vector<unsigned char> affordable(groups.size(), 0);
    for (size_t g = 0; g < groups.size(); ++g)
        affordable[g] = clusterGroupAffordable(groups[g].error, allowed) ? 1 : 0;
    return clusterCutFromAffordable(clusters, affordable, out);
}

/// ONE SURFACE CARD, in the MESH'S OWN SPACE — the engine-facing copy of the
/// document's `iris::MeshCard` (SURFACE-CACHE-1a).
///
/// A card is the DESCRIPTION of a capture, not a capture: where to stand an
/// orthographic camera, how wide to make it, how deep to let it see and which
/// LOD level to raster for it. The six axis directions and the (u, v) frame of
/// each are THE CONTRACT below (`cardAxisDirection` / `cardAxisU` /
/// `cardAxisV`), which the engine's capture caches as Ogre vectors and the
/// document restates in `iris::MeshCard`; a suite asserts the two tables agree and that
/// every frame is right-handed (u x v = the axis) — because the bake, the
/// capture and the read all parameterise the same rectangle and a disagreement
/// of one sign is a mirrored card nobody would see until phase 4.
struct MeshCardDesc {
    unsigned char axis = 0;       ///< 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z
    unsigned char lodLevel = 0;   ///< the level whose baked error is under this card's texel
    Vec3  origin;                 ///< the CENTRE of the card's box, mesh space
    float halfU = 0.0f;           ///< half-sizes of the rectangle along the axis frame, metres
    float halfV = 0.0f;
    float halfDepth = 0.0f;       ///< half the depth range the capture must cover
};

/// THE SIX AXIS FRAMES — THE CONTRACT, in the one header both halves include.
///
/// The bake writes a card's rectangle in this frame; the capture stands its
/// camera up from it; phase 4's read projects a world hit back through it. Three
/// readers of one parameterisation is exactly the shape a sign error hides in.
///
/// THERE ARE TWO COPIES OF THIS TABLE AND THERE HAVE TO BE: the document half
/// is `iris::MeshCard::axisDirection/axisU/axisV`, and a document header may not
/// include an engine one (they are two libraries that never link each other).
/// What there is instead of one copy is ONE GUARD — `meshbake.cards` includes
/// both headers and asserts, axis by axis, that the two tables are equal and
/// that every frame is RIGHT-HANDED (u x v = the axis). A drift between them is
/// a mirrored card, which is invisible until something reads one.
///
/// (THE +Y ROW WAS THE ONE THAT WAS NOT, until SURFACE-CACHE-1b: its v was
/// (0,0,1) against every other pair's shared v, and `u x v` came out as the
/// NEGATED axis. A left-handed frame is not a wrong rectangle — the rect is
/// centred, so no baked byte moved — but it is a reflection, and a capture
/// camera built from a reflection is not a rotation at all.)
inline Vec3 cardAxisDirection(unsigned axis) {
    switch (axis) {
    case 0: return Vec3{ 1, 0, 0 };
    case 1: return Vec3{ -1, 0, 0 };
    case 2: return Vec3{ 0, 1, 0 };
    case 3: return Vec3{ 0, -1, 0 };
    case 4: return Vec3{ 0, 0, 1 };
    default: return Vec3{ 0, 0, -1 };
    }
}
inline Vec3 cardAxisU(unsigned axis) {
    switch (axis) {
    case 0: return Vec3{ 0, 0, -1 };
    case 1: return Vec3{ 0, 0, 1 };
    case 2: return Vec3{ 1, 0, 0 };
    case 3: return Vec3{ -1, 0, 0 };
    case 4: return Vec3{ 1, 0, 0 };
    default: return Vec3{ -1, 0, 0 };
    }
}
inline Vec3 cardAxisV(unsigned axis) {
    switch (axis) {
    case 0:
    case 1: return Vec3{ 0, 1, 0 };
    case 2:
    case 3: return Vec3{ 0, 0, -1 };
    default: return Vec3{ 0, 1, 0 };
    }
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
    // `lodBounds[i]` is level i+1's BOUND, a LENGTH IN MESH UNITS: the largest
    // STORED error among its clusters (document/assets/mesh.h) - each the DAG's
    // max(children, this step) over a step's measured deviation from its INPUT
    // (import/atomdag/measure.h), NOT a distance from level 0: the level's drift
    // from level 0 is within kLevelDriftOverBound times it (the bake's ship law,
    // atom2.one_step_drift_bound), which is what a consumer needing a distance
    // from level 0 multiplies by (the ray origin, below). Monotonically
    // non-decreasing. It is the currency of the whole
    // program, and every consumer spends it the same way — by stating the
    // world-space deviation IT can afford and taking the coarsest level below it:
    //   * the VIEW turns a PIXEL budget into that deviation at its own live
    //     lens and viewport height (`kLodBudgetPixels`, and the strategy in
    //     OgreMesh.cpp),
    //   * a VOXELISER turns its own CELL into it through a measured fraction
    //     (`kCascadeLodCellFraction` in OgreGi.cpp — a binary occupancy test
    //     moves its boundary with the deviation, so the fraction is small and
    //     it was measured on the picture, not argued), and
    //   * a CARD turns its own texel into it, at bake time.
    //
    // `lodErrors[i]` is what the SIMPLIFIER claimed about that level. NO CONSUMER
    // READS IT: it is carried across the boundary so a diagnostic (the render
    // monitor, a bake report) can show both numbers side by side, which is how
    // the margin on the measurement stays honest.
    std::vector<std::vector<unsigned>> lodIndices;
    std::vector<float>                 lodErrors;
    std::vector<float>                 lodBounds;

    // ---- SURFACE-CACHE phase 1: the mesh's CARD LIST -----------------------
    //
    // Built at IMPORT by MeshBake beside the chain above (SURFACE-CACHE-1a) and
    // handed across unchanged: the cards are in MESH SPACE, so one list serves
    // every instance of the mesh at any transform, exactly as the LOD chain
    // does. Empty for every mesh that gets none — a skinned mesh (its surface
    // moves, so a card baked against the bind pose is a lie), a line mesh, a
    // mesh with no usable area, a bake made before cards existed.
    std::vector<MeshCardDesc> cards;
    /// The fraction of the mesh's sampled surfels covered by at least one card,
    /// as the generator measured it (occlusion included). 0 with no cards.
    float cardCoverage = 0.0f;

    // ---- ATOM stage 2: the mesh's CLUSTER DAG (see `clusterCut` above) -----
    //
    // Built at IMPORT by MeshBake beside the chain; the mirror expands the bake's
    // meshlet-local form into ONE global index stream, all clusters' triangles
    // concatenated in cluster order, so a cluster is the contiguous range
    // [firstIndex, firstIndex + indexCount) of `clusterIndices`. `buildMeshV2`
    // uploads that stream as one extra index buffer per mesh (the input of stage
    // 3's cut). All three empty for a mesh with no DAG — every skinned mesh, and
    // every mesh under 256 triangles, which is one cluster, i.e. level 0.
    std::vector<unsigned>         clusterIndices;
    std::vector<MeshCluster>      clusters;
    std::vector<MeshClusterGroup> clusterGroups;

    size_t vertexCount() const { return positions.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }
    /// The COARSEST level that still stands in for this mesh when the consumer
    /// can afford a world-space deviation of `allowed` —
    /// THE RULE ITSELF IS `lodLevelForWorldError` ABOVE, stated once and shared
    /// with the engine's voxel gather (OgreScene::cascadeGatherInputs). This overload
    /// is the document-side convenience: it clamps to the levels this mesh
    /// actually carries.
    /// (`lodLevelCount` and `lodLevelIndices` used to sit here and are DELETED —
    /// ATOM inventory row AT-DEAD: their only callers were the suite that tested
    /// them. `lodIndices` is public and the engine's own level walk is
    /// `buildMeshV2`'s `accepted` list, which never went through either.)
    size_t lodForWorldError(float allowed) const {
        return lodLevelForWorldError(lodBounds, allowed, lodIndices.size());
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
/// THE BUDGET: ONE PIXEL of MEASURED geometric deviation. Derived, and the
/// derivation is short because the quantity is now honest (ATOM P1's AT-A5 — the
/// old text had to hedge with "the simplifier's combined, >= geometric error",
/// which was not true in either direction).
///
/// One pixel is the largest budget that cannot be seen: the deviation is a
/// displacement of the SILHOUETTE and of shaded normals, and a displacement under
/// one pixel is under the sampling rate of the image it lands in. Half a pixel
/// would be Nyquist-strict and is not needed — the bound is a MAXIMUM over the
/// surface while what an eye integrates is the average, and the bound already
/// carries the sampling-gap margin. Two pixels may be visible on a moving silhouette:
/// that frame-to-frame delta is ATOM-BAKE-2's dolly gate's subject, and its arms at 2 and
/// 3 px print it (ATOM-3 stage 1).
///
/// THIS IS EPIC'S THRESHOLD, NOT EVERY TIER'S (ATOM-3 stage 1): the view's threshold is the
/// World Mode's `atomPixelError` row — Low 4, Medium 3, High 2, Epic 1 px (the lead's
/// numbers, 2026-10-19) — pushed to Scene::setPixelTolerance; a scene that never set one
/// draws at this. The id pass's cut takes it times the LOD bias, and the stock chain takes
/// it through the VIEW camera's own LOD factor (Ogre::Camera::setLodBias, which the world-
/// error strategy reads) so a shadow map's camera keeps one texel: the map's texel is
/// already its tier's lever (the shadowResolution / shadowFilter rows).
///
/// WHAT IT IS WORTH, measured on the shipped chains: at one pixel on a 1080-line
/// 45-degree view, the sphere's level 1 (bound 0.032 m) is taken at a bounding-
/// sphere radius of about 40 px and its level 2 (0.071) at about 18 px — i.e. the
/// levels arrive while the object is still a recognisable shape on screen, which is
/// what makes the triangle saving real rather than notional. A tolerance whose
/// savings arrive only once the object is 4 px across would not be a tolerance.
constexpr float kLodBudgetPixels = 1.0f;

// THE RAY TIER'S NEAR COPY IS LEVEL 0 (PHOTON-7 stage 6, ONE RAY LEVEL PER MESH): every ray - the gather's, the sun
// contact's, the cards' sun, the reflections' - traces the authored triangles near, so a contact meets its
// reflection and the traced geometry is a function of the world, never of where a camera stands (law L1). The 2x
// distance band that re-fitted a coarser level per view (one footprint of tolerance) is deleted; measured on Bistro
// Exterior at the owner's pose: 211.8 MB of bottom levels against the band's 138.5 MB (1.53x), 0 refits on a walk.

// THE RAY ORIGIN - ONE LAW FOR EVERY RAY THAT LEAVES A SURFACE (PHOTON-7 stage 6, READ_B finding 3; the GLSL half is
// src/rayquery/include/jah_ray_origin.glsl). A ray traces level 0 (above) while the surface it starts on is the one the
// VIEW DREW (Atom's cut, a stock item's level: the reflection, the gather's probes, the sun contact) or a CARD CAPTURED
// (the bake's level for its texel: the card sun), so with tMin 0 the origin is lifted along that surface's GEOMETRIC
// normal by the point's own error (its float bound; a card texel's reconstruction) plus THE SURFACE'S BOUND AGAINST
// LEVEL 0, measured the way the LOD rule measures it:
//   * A VIEW'S PIXEL: the tolerance (Scene::pixelTolerance x the LOD bias - what the id pass's cut and the stock
//     thresholds both take) x one sample's footprint per metre (sampleFootprintPerspective at 1 m: |rayDown| / H) x the
//     point's EUCLIDEAN distance from the eye. The strategy (OgreMesh.cpp) and the cut (JahClusterCut.glsl) both judge
//     an object at its bounding sphere's nearest distance |centre - eye| - radius, and every point of the object is at
//     least that far; the forward depth the old lifts used is 1.24x short at a 16:9 45-degree frame's side edge and
//     1.31x at its corner. A STOCK level is HELD down to (1 - band) of its threshold on a watched view
//     (OgreLodStrategyPrivate.inl: finer only once the value falls under threshold - band), so the bound is over
//     (1 - band); Atom's cut has no band (OgreAtomIdPass.cpp, D4) and lies inside the same bound.
//   * A CARD: its captured level's bound in world units (CardRec::levelBound).
// THE BOUND IS THE STORED ERROR x kLevelDriftOverBound. What the LOD rule spends is the bake's STORED error, Nanite's
// max(children, this step) over each step's measured deviation from its INPUT (import/atomdag/measure.h), not a
// distance from level 0: the uniform cut's drift from level 0 measured 1.154x its stored error on the rough sheet and
// 1.117x on the curved shell (atom2.one_step_drift_bound's fixtures, 2026-10-23), and the bake's SHIP law is that it
// stays within 2x at every depth. That law is this constant; the row asserts it (tests/meshbake/atom2_rows.h).
// At Epic's 1 px the view's term is 2.2 footprints on axis (the sun contact's tolerance + 1 was 2, and its "+1"
// margin for a convex caster's sign is this term now - the float bound covers a start exactly on level 0).
constexpr float kLevelDriftOverBound = 2.0f;

/// The view's drawn-surface term, in sample FOOTPRINTS at the point's Euclidean distance (the shaders multiply by
/// |rayDown| / H and the distance - jahViewDrawnBound): `tolerance` = pixel tolerance x LOD bias, `lodBand` = the
/// view's switch band (ChainDesc::lodHysteresis: 0 on a capture, jahLodHysteresis on a watched view).
inline float rayOriginDrawnFootprints(float tolerance, float lodBand) {
    if (!(tolerance > 0.0f)) return 0.0f;
    const float band = (lodBand > 0.0f && lodBand < 1.0f) ? lodBand : 0.0f;
    return kLevelDriftOverBound * tolerance / (1.0f - band);
}

/// A captured level's term (the card sun): its stored bound in world units, as a bound against level 0.
inline float rayOriginLevelLift(float levelBoundWorld) {
    return levelBoundWorld > 0.0f ? kLevelDriftOverBound * levelBoundWorld : 0.0f;
}

/// THE RAY INSTANCE MASK (ATOM-FARBLAS-1, A5b §4). ONE top-level structure holds
/// every traced object TWICE: a NEAR copy over the mesh's level 0,
/// carrying the per-consumer bits (audit C-15) plus `kRayMaskNear`, and a FAR
/// copy over the mesh's coarsest level carrying `kRayMaskFar` ALONE. A launch
/// names what it may hit with its cull mask, so the split costs a bit, not a
/// second structure:
///   * near launches (reflections, a probe gather ray inside its near length,
///     a sun-contact ray) trace `kRayMaskNearField` or a subset of its bits;
///   * the gather's far query (a ray that escaped its near length) traces
///     `kRayMaskFar` from the near end to the far plane.
/// No near launch can hit a far copy and the far launch hits nothing else, so
/// one object never answers one ray twice.
constexpr unsigned kRayMaskCaster = 0x01u;   ///< casts a shadow
constexpr unsigned kRayMaskMover = 0x02u;    ///< the document says it moves
constexpr unsigned kRayMaskStill = 0x04u;    ///< still world (not a mover)
constexpr unsigned kRayMaskNear = 0x08u;     ///< every near copy
constexpr unsigned kRayMaskFar = 0x10u;      ///< every far copy, and nothing else
/// A SHADOW-CASTING MOVER'S NEAR COPY, and nothing else (PHOTON-CARDS-4): the
/// surface cache's mover-shadow launch traces this bit alone. A launch's mask
/// matches an instance on ANY common bit, so "a mover AND a caster" cannot be
/// asked with bits 0 and 1 — it is its own bit. No other launch names it.
constexpr unsigned kRayMaskMoverCaster = 0x20u;
/// A SHADOW-CASTING STILL OBJECT'S NEAR COPY (ATOM-S3-CARDCAP): the surface cache's
/// still-world sun launch traces this bit alone — the card's still sun term, one ray
/// per card texel (the MoverCaster bit's reason: "a caster AND still" is its own bit).
constexpr unsigned kRayMaskStillCaster = 0x40u;
constexpr unsigned kRayMaskNearField = kRayMaskCaster | kRayMaskMover | kRayMaskStill | kRayMaskNear;

}}  // namespace jahshaka::engine
