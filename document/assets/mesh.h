/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef MESH_H
#define MESH_H

#include "document/export.h"

#include "core/math/quat.h"
#include "core/math/vec.h"
#include <string>
#include <vector>

#include "irisglfwd.h"
#include "document/animation/skeletalanimation.h"
#include "core/geometry/boundingsphere.h"
#include "core/geometry/aabb.h"
#include "document/assets/vertexlayout.h"
#include "document/assets/vertexbuffer.h"
#include <cstdint>

namespace iris
{

class BoundingSphere;

enum class PrimitiveMode
{
    Triangles,
    Lines,
    LineLoop,
	LineStrip
};

/// ONE SURFACE CARD — an axis-aligned orthographic capture rectangle over a
/// patch of the mesh's surface, in the MESH'S OWN SPACE (SURFACE-CACHE phase 1,
/// SPECS/SURFACE_CACHE_ASSESSMENT.md §2/§7).
///
/// A card is not geometry and not a texture: it is the DESCRIPTION of a capture
/// that phase 2 will run — where to put an orthographic camera, how wide to
/// make it, how deep to let it see, and which LOD level of the mesh to raster
/// for it. Lumen's shape exactly (surfels clustered, each cluster one
/// axis-aligned direction, a 6-face box when clustering has nothing to say);
/// the list is built ONCE at import, in the bake, beside the LOD chain.
///
/// EVERYTHING IS IN MESH SPACE and stays there: an instance's world transform
/// is applied by whoever captures, so one mesh's card list serves every
/// instance of it, at any scale, exactly as the LOD chain does.
struct IRIS_DOCUMENT_API MeshCard
{
    /// The six axis directions a card can look FROM, in this order:
    /// 0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z. The capture camera sits
    /// on the `+axisDirection()` side of `origin` and looks back along it, so a
    /// surface whose normal has a POSITIVE dot with `axisDirection()` faces the
    /// card.
    static constexpr int kAxisCount = 6;

    static Vec3 axisDirection(int axis)
    {
        switch (axis) {
        case 0: return Vec3(1, 0, 0);
        case 1: return Vec3(-1, 0, 0);
        case 2: return Vec3(0, 1, 0);
        case 3: return Vec3(0, -1, 0);
        case 4: return Vec3(0, 0, 1);
        default: return Vec3(0, 0, -1);
        }
    }
    /// The card plane's basis. Fixed per axis and right-handed with the axis
    /// direction (u x v = the axis direction), so a card's (u, v) parameters
    /// mean the same thing in the bake, in the capture and at the read — there
    /// is no per-card rotation to store or to get wrong.
    ///
    /// THE +Y ROW WAS LEFT-HANDED UNTIL SURFACE-CACHE-1b (2026-09-21), and the
    /// sentence above was the only place that said otherwise: axis 2 had
    /// u = (1,0,0) with v = (0,0,1), whose cross product is (0,-1,0) — the
    /// NEGATED axis, and the only one of the six that disagreed. Every other
    /// pair shares its v and mirrors its u (+X/-X share (0,1,0), +Z/-Z share
    /// (0,1,0)); +Y/-Y did not, which is exactly the shape of the mistake.
    /// v for +Y is now (0,0,-1), matching -Y, so the six frames are uniform and
    /// every one is right-handed — asserted by the convention case in
    /// `gi.card_capture`, which is what would have caught it.
    ///
    /// NO BAKED BYTE MOVES WITH THIS, and that is arithmetic rather than hope:
    /// a card's rectangle is stored as a CENTRE (`origin`) and two SYMMETRIC
    /// half-sizes, and `MeshBake::fromBounds` computes the centre as
    /// `U*(uMin+uMax)/2 + V*(vMin+vMax)/2 + D*(...)` over the surfels' own
    /// min/max — flip the sign of V and the min and the max swap, so the
    /// centre is the same world point and the half is the same length. The
    /// clustering, the coverage raster and the LOD pick read the frame the same
    /// way on both sides of the flip. Hence `bake-output: unchanged`.
    static Vec3 axisU(int axis)
    {
        switch (axis) {
        case 0: return Vec3(0, 0, -1);
        case 1: return Vec3(0, 0, 1);
        case 2: return Vec3(1, 0, 0);
        case 3: return Vec3(-1, 0, 0);
        case 4: return Vec3(1, 0, 0);
        default: return Vec3(-1, 0, 0);
        }
    }
    static Vec3 axisV(int axis)
    {
        switch (axis) {
        case 0:
        case 1: return Vec3(0, 1, 0);
        case 2:
        case 3: return Vec3(0, 0, -1);
        default: return Vec3(0, 1, 0);
        }
    }

    /// 0..5 — one of the six directions above.
    std::uint8_t axis = 0;
    /// The LOD level (0 = the authored geometry, i = `lodIndices[i - 1]`) whose
    /// simplifier error is below this card's TEXEL — the same rule ATOM-2's
    /// voxeliser picks a level by, with the card texel in the cell's place.
    std::uint8_t lodLevel = 0;
    /// The CENTRE of the card's box, mesh space. The capture's near plane is
    /// `origin + axisDirection(axis) * halfDepth`, its far plane
    /// `origin - axisDirection(axis) * halfDepth`.
    Vec3 origin;
    /// Half-sizes of the capture rectangle along `axisU` and `axisV`, metres.
    float halfU = 0.0f;
    float halfV = 0.0f;
    /// Half the depth range the capture must cover, metres.
    float halfDepth = 0.0f;
    /// The fraction of the mesh's sampled surfels THIS card sees — facing it,
    /// inside its rectangle and its depth range, and not hidden behind nearer
    /// surface of the same mesh. Cards overlap, so these do not sum to the
    /// mesh's own `cardCoverage`.
    float coverage = 0.0f;
};

/// THE PER-MESH CLUSTER DAG — a bake product (ATOM-2: irisgl/import/atomdag/, the builder of our own; build.h
/// states the design, D1-D8), beside the LOD chain: the chain is what the voxeliser, the cards, the far BLAS and the
/// casters read (until the level table becomes a cut of this DAG, ATOM-2 stage 5); the DAG is what the GPU cut reads.
///
/// WHAT IT IS: level 0 split into CLUSTERS of at most 128 triangles and 256 vertices; each level's clusters grouped
/// 8-32, each group simplified to about half and re-split, until one cluster is left, then an EMPTY TOP group. Every
/// cluster is a member of exactly one group (`group`) and was produced by exactly one group (`refined`, -1 for a
/// level-0 cluster); a group with no produced cluster draws nothing when it is affordable (the empty top: a thing
/// under a pixel stops drawing). THE RENDER TEST is stated once in C++ in jahshaka/engine/Mesh.h (`clusterCut`) and
/// once in GLSL (JahClusterCut.glsl): a cluster is drawn iff its own group's error is NOT affordable and its
/// `refined` group's IS (or it is level 0).
///
/// THE ERROR THE RULE READS IS MEASURED: a group's `error` = max(its members' stored error, the measured one-step
/// deviation of its output from its input — exact both ways, island-capped), in mesh units, monotone by
/// construction; `estimate` is the simplifier's own number (a diagnostic). `centre`/`radius` of a group is its LOD
/// sphere: the sphere enclosing every member's LOD sphere (a leaf's is its own) — so a parent is never further
/// from the eye than its child, and the rule selects exactly one cut.
///
/// THE ON-DISK SHAPE IS MESHLET-LOCAL: per cluster a slice of `vertices` (the mesh vertices it uses, first-use
/// order) and a slice of `triangles` (three 8-bit indices into that slice per triangle). The engine hand-off expands
/// it to one global index stream (irisgl/mirror/scenemirror.cpp toMeshData).
///
/// EMPTY for a mesh that gets none: a skinned mesh, a line mesh, a malformed one.
struct MeshClusterDag
{
    struct Cluster
    {
        std::uint32_t vertexOffset = 0;     ///< first entry of this cluster in `vertices`
        std::uint32_t triangleOffset = 0;   ///< first TRIANGLE of this cluster in `triangles` (3 bytes each)
        std::uint16_t vertexCount = 0;
        std::uint16_t triangleCount = 0;
        std::int32_t  group = -1;           ///< the group this cluster is a MEMBER of
        std::int32_t  refined = -1;         ///< the group whose simplification PRODUCED it; -1 = level 0
        float   centre[3] = { 0.0f, 0.0f, 0.0f };   ///< culling sphere, mesh space
        float   radius = 0.0f;
    };
    struct Group
    {
        std::int32_t depth = 0;             ///< the DAG level the group was formed at
        float  centre[3] = { 0.0f, 0.0f, 0.0f };    ///< the simplified bounds' sphere, mesh space —
        float  radius = 0.0f;         ///< ...grown at bake to CONTAIN every child group's sphere
        float  error = 0.0f;          ///< MEASURED, mesh units, monotone, finite (the reader refuses anything else)
        float  estimate = 0.0f;       ///< the simplifier's own error (a diagnostic)
    };

    std::vector<Cluster> clusters;
    std::vector<Group>   groups;
    std::vector<std::uint32_t> vertices;        ///< per-cluster local -> mesh vertex, concatenated
    std::vector<std::uint8_t> triangles;   ///< 8-bit local indices, 3 per triangle, concatenated

    bool isEmpty() const { return clusters.empty(); }
    int  triangleCount() const { return int(triangles.size() / 3); }
    /// The mesh-vertex index of corner `k` (0..2) of local triangle `t` of cluster `c`.
    std::uint32_t vertexOf(const Cluster &c, int t, int k) const
    {
        const std::uint8_t local = std::uint8_t(triangles.at(int(c.triangleOffset + std::uint32_t(t)) * 3 + k));
        return vertices.at(int(c.vertexOffset) + int(local));
    }
};

// CPU-side mesh: geometry buffers, skeleton, animations, bounds and the picking
// TriMesh. The GL half (VAO/draw) died with the legacy renderer at step 14; the
// engine mirror converts these buffers into engine meshes each time one changes.
class IRIS_DOCUMENT_API Mesh
{
    SkeletonPtr skeleton;
    std::map<std::string, SkeletalAnimationPtr> skeletalAnimations;

	std::vector<VertexBufferPtr> vertexBuffers;
	IndexBufferPtr idxBuffer;

public:
    // Triangles because that is what every construction site sets it to
    // (meshbake.cpp:344, linemeshbuilder.cpp:28, gizmomeshes.cpp:97,
    // translationgizmo.cpp:495) and what the bake writes back out.
    PrimitiveMode primitiveMode = PrimitiveMode::Triangles;
    // `usesIndexBuffer` is written by both constructors and so satisfies
    // source.member_init without this — but the import's aiMesh build
    // (meshFromAiMesh, import/scenebuilder.cpp) writes it AFTER its
    // `if (!mesh->HasPositions()) return out;` early return, which is the exact
    // shape the rule's `a helper can grow an early return` is about.
    bool usesIndexBuffer = false;

    BoundingSphere boundingSphere;
	AABB aabb;

    int numVerts;
    int numFaces;

    TriMesh* triMesh;

    /// THE WHOLE-MESH LEVEL TABLE (ATOM-2 D10, ONE SIMPLIFIER): at most eight UNIFORM-DEPTH CUTS of the cluster DAG
    /// (`clusterDag` below), built at IMPORT by MeshBake::buildLevelTable and carried in the .jmb bake; empty for a mesh
    /// with no DAG. The stock casters, the voxeliser, the surface cache, the ray BLAS and the cards read it.
    ///
    /// `lodIndices[i]` is LEVEL i+1: the triangles of every DAG cluster alive at one depth (born at or before it, its
    /// own group forming at or after it — the builder's pool there, a crack-free surface), as one index list over the
    /// SAME vertex buffers as `idxBuffer` (level 0; a card quad's corners are vertices the DAG appended): one vertex
    /// buffer, N index buffers keeps every level of a mesh inside ONE draw call downstream.
    ///
    /// `lodBounds[i]` is level i+1's BOUND: the largest stored error among those clusters — each the measured
    /// one-step deviation chain of the DAG (import/atomdag/measure.h), monotone by construction — as a LENGTH in mesh
    /// units, positive and non-decreasing with i. Divided by the view distance it is a screen-space error; compared
    /// against a world-space cell size it answers "is this level fine enough for a voxel of that size". The one
    /// selection rule over it is `lodLevelForWorldError` (jahshaka/engine/Mesh.h).
    ///
    /// `lodErrors[i]` is a DIAGNOSTIC: the largest simplifier estimate of the groups that made level i+1's clusters.
    /// Nothing reads it to choose a level.
    ///
    /// The three arrays have one entry per level above 0.
    std::vector<std::vector<std::uint32_t>> lodIndices;
    std::vector<float>            lodErrors;
    std::vector<float>            lodBounds;

    /// SURFACE-CACHE phase 1 — the mesh's card list, built at IMPORT by
    /// MeshBake (beside the chain above) and carried in the .jmb bake, and
    /// built at CREATION for a primitive (Mesh::loadMesh). Empty for every mesh
    /// that gets none: a skinned mesh (its surface moves, so a card baked
    /// against the bind pose is a lie — Epic's own limit), a line mesh, a mesh
    /// with no usable area, or a bake produced before cards existed.
    ///
    /// Nothing reads a card yet: the capture is phase 2 and the read is
    /// phase 4. What this list is, is the BUDGET those phases spend — the cost
    /// of a capture is fixed per card (SURFACE-CACHE-0's measurement), so the
    /// number and the shape of the cards authored here is the number and the
    /// shape of the work they will do.
    std::vector<MeshCard> cards;

    /// The fraction of the mesh's sampled surfels covered by AT LEAST ONE card
    /// — the quality of the list above, measured by the generator against the
    /// real surface (occlusion included) and carried so a consumer never has to
    /// re-derive it. 0 when there are no cards.
    float cardCoverage = 0.0f;


    /// ATOM stage 2 — the mesh's CLUSTER DAG, built at IMPORT by MeshBake beside
    /// the chain (see MeshClusterDag above). Empty for every mesh that gets none.
    MeshClusterDag clusterDag;

    /// CPU-side geometry, read-only. The engine mirror and importers convert from
    /// these.
    const std::vector<VertexBufferPtr>& getVertexBuffers() const { return vertexBuffers; }
    IndexBufferPtr getIndexBuffer() const { return idxBuffer; }

    TriMesh* getTriMesh()
    {
        return triMesh;
    }


    bool hasSkeleton();
    SkeletonPtr getSkeleton();

    void addSkeletalAnimation(std::string name, SkeletalAnimationPtr anim);
    std::map<std::string, SkeletalAnimationPtr> getSkeletalAnimations();
    bool hasSkeletalAnimations();

    // (`loadMesh`, `pinLoadPaths`, `clearLoadCache` and `loadCacheSize` are
    // DELETED — ATOM P2, 2026-09-22. They were how the twelve primitives, the
    // Ground, the avatar's cube and the preview spheres were born: an assimp
    // parse of a Qt resource on first use, a weak cache with a pin over it for
    // the shipped paths, and the surface-card generator run AT CREATION for ~6 ms
    // per node on the thread that draws — with no LOD chain, because a chain was
    // "ATOM's decision to make". Those meshes are BAKED LIBRARY ASSETS now
    // (jahshaka/src/services/primitiveassets.h): one import, one bake, a real
    // chain, and the document holds a reference to the asset like it does for
    // every model a user imports. A Mesh is built from a parsed scene (the
    // importers) or read from a bake; nothing loads one from a path.)



    //assumed ownership of vertexLayout
    static Mesh* create(void* data,int dataSize,int numElements,VertexLayout* vertexLayout);
	static MeshPtr create(VertexLayout vertexLayout);
	static MeshPtr create();

	Mesh();

    /**
     *
     * @param data
     * @param dataSize
     * @param numElements number of vertices
     * @param vertexLayout
     */
    Mesh(void* data,int dataSize,int numElements,VertexLayout* vertexLayout);

    ~Mesh();

	void setVertexCount(const unsigned int count);
    void setSkeleton(const SkeletonPtr &value);

    PrimitiveMode getPrimitiveMode() const;
    void setPrimitiveMode(const PrimitiveMode &value);

    void clearVertexBuffers();
	void addVertexBuffer(VertexBufferPtr vertexBuffer);
	void setIndexBuffer(IndexBufferPtr indexBuffer);

	AABB getAABB(){return aabb;}
	BoundingSphere getBoundingSphere() { return boundingSphere; }

};

}

#endif // MESH_H
