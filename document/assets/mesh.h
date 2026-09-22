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

#include "core/math/quat.h"
#include "core/math/vec.h"
#include <QString>
#include <QStringList>
#include <QColor>
#include <QVector>

#include "irisglfwd.h"
#include "document/animation/skeletalanimation.h"
#include "core/geometry/boundingsphere.h"
#include "core/geometry/aabb.h"
#include "document/assets/vertexlayout.h"
#include "document/assets/vertexbuffer.h"

// No assimp in this header (ENGINEERING_DEBT L4 part 2, 2026-09-09): every use
// below is by POINTER, so the two forward declarations are all a consumer needs;
// the TUs that read aiMesh/aiScene include assimp themselves.
struct aiScene;

struct aiMesh;

namespace iris
{

class BoundingSphere;

struct MeshMaterialData
{
    QColor diffuseColor;
    QColor specularColor;
    QColor ambientColor;
    QColor emissionColor;
    float shininess = 0.0f;

    QString diffuseTexture;
    QString specularTexture;
    QString normalTexture;
    QString hightTexture;

    bool hasEmbeddedDiffTexture = false;
    bool hasEmbeddedSpecularTexture = false;
    bool hasEmbeddedNormalTexture = false;
    bool hasEmbeddedHightTexture = false;

    // glTF 2.0 metallic-roughness, read at import (GLB importer fix phase 0).
    // hasPbr is true when the source material is a PBR one at all — any glTF
    // material qualifies, in whichever workflow it was authored; importers then
    // build an iris::PbrMaterial from these instead of faking a legacy
    // Blinn-Phong material out of assimp's lossy shininess back-conversion.
    //
    // THE DEFAULTS ARE A POLICY, and they are DIELECTRIC (2026-09-08).
    // glTF says an omitted `metallicFactor` inside a PRESENT
    // `pbrMetallicRoughness` block is 1.0, and the importer honours that
    // exactly (a metallic car imports as metal and looks dark in a scene with
    // nothing to reflect — physics, not a defect). But a material that states
    // NO workflow at all — no metallic-roughness block and no
    // KHR_materials_pbrSpecularGlossiness — is not a statement that it is a
    // mirror; reading it as one is how models imported black. Such a material,
    // and any importer that fills none of these in, lands here: a plain
    // dielectric, half rough, which is the only defensible "unknown surface".
    bool hasPbr = false;
    QColor baseColorFactor = QColor(255, 255, 255, 255);
    float metallicFactor = 0.0f;
    float roughnessFactor = 0.5f;
    /// KHR_materials_unlit: the source says this surface is NOT lit. Imports as
    /// PbrMaterial shadingModel 1 (the engine's Unlit family) — see
    /// BuiltinMaterials::fromMeshData for what carries the colour there.
    bool unlit = false;
    QString baseColorTexture;
    QString metallicTexture;    // split from the packed MR map (blue channel)
    QString roughnessTexture;   // split from the packed MR map (green channel)
    QString emissiveTexture;

    // ---- Specular / fresnel workflows (MATERIAL_GAPS_SPEC GAP 1) ----------
    //
    // The renderer has three PBR workflows, so a source authored in one of the
    // two SPECULAR ones no longer has to be converted to metallic-roughness to
    // arrive. The conversion (specularGlossinessToMetallicRoughness) is still
    // here and still correct — it is now the FALLBACK, for targets that cannot
    // carry a workflow (web export), not the import path.
    //
    // 0 Metallic (the default, and what a metallic-roughness glTF is),
    // 1 Specular (legacy KHR_materials_pbrSpecularGlossiness — native, lossless),
    // 2 Specular-as-Fresnel (KHR_materials_specular — the pin's own docs call
    //   this "what most PBRs mean by specular").
    int    workflow = 0;
    /// kS. From KHR_materials_specular's specularFactor, or a spec-gloss
    /// material's specularFactor. White is inert.
    QColor specularFactor = QColor(255, 255, 255, 255);
    /// F0 straight from KHR_materials_specular's specularColorFactor. Only
    /// meaningful with `useFresnelColor`; otherwise `ior` supplies F0.
    QColor fresnelFactor = QColor(10, 10, 10, 255);
    bool   useFresnelColor = false;
    /// KHR_materials_ior. 1.5 is the extension's own default and ours.
    float  ior = 1.5f;
    /// The SPECULAR / spec-gloss map. It binds to the renderer's shared
    /// metallic/specular texture unit — the same unit metallicTexture uses —
    /// so exactly one of the two is ever set, decided by `workflow`. Before
    /// the workflow switch existed this map had no home at all and was dropped
    /// with a warning.
    QString specularMapTexture;
};

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
struct MeshCard
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
    quint8 axis = 0;
    /// The LOD level (0 = the authored geometry, i = `lodIndices[i - 1]`) whose
    /// simplifier error is below this card's TEXEL — the same rule ATOM-2's
    /// voxeliser picks a level by, with the card texel in the cell's place.
    quint8 lodLevel = 0;
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

/// THE PER-MESH SIGNED DISTANCE FIELD — a bake product (ATOM P2, SUB-S5-SDF),
/// in the MESH'S OWN SPACE like the cards and for the same reason: one field
/// serves every instance at any transform.
///
/// WHAT IT IS: a uniform grid of int8 distances, jump-flooded at IMPORT over
/// LEVEL 0's triangle soup, negative inside and positive outside. `scale` is the
/// distance `|value| == 127` stands for, so a cell's distance in mesh units is
/// `value / 127 * scale`, saturating beyond it — the sign stays right out there,
/// which is all a coarse occupancy test needs, and the band near the surface is
/// where the resolution goes.
///
/// WHY THE RESOLUTION IS TIED TO THE CHAIN: the cell can never be finer than the
/// geometry is HONEST (`meshbake.cpp`'s `kSdfCellPerBound`, four times level 1's
/// measured bound). A field finer than the surface it was measured from would
/// record detail the bake has already said it cannot promise.
///
/// THE SIGN OF AN OPEN MESH is the nearest triangle's geometric normal, not a
/// ray-parity test, so a plane or a hemisphere gets a two-sided OFFSET field
/// rather than a refusal. That is the honest answer for geometry with no inside:
/// a consumer reading occupancy off such a mesh is asking the wrong question,
/// and a consumer reading short-range occlusion or a distance to the surface —
/// which is every consumer named in SUB-S5-SDF — gets exactly what it wants.
///
/// NOTHING READS IT YET. The consumers are Photon's (coarse-cascade occupancy by
/// min-composite, the field's short-range occlusion, the far occluder past the
/// last cascade) and they arrive in their own phases; this is the PRODUCT, baked
/// once under the same format bump as the measured LOD bound.
struct MeshSdf
{
    /// The FORMAT's ceiling on cells per axis, here beside the struct for the
    /// same reason MeshCard::kAxisCount is: the bake's reader has to refuse a
    /// bigger grid, and the generator has to stay under the same number.
    /// 64^3 int8 = 256 KB, the most a per-mesh product may cost.
    static constexpr int kMaxDim = 64;

    /// Cells per axis. All three zero = this mesh has no field (skinned, not
    /// triangles, no area) — which costs six bytes and is an honest answer.
    quint16 dim[3] = { 0, 0, 0 };
    /// The CENTRE of cell (0, 0, 0), mesh space.
    Vec3 origin;
    /// The cell's edge, metres. CUBIC on purpose: a distance field with
    /// anisotropic cells is not a distance field.
    float cell = 0.0f;
    /// The distance `|value| == 127` represents, metres.
    float scale = 0.0f;
    /// dim[0]*dim[1]*dim[2] int8 values, x fastest, then y, then z.
    QByteArray values;

    bool isEmpty() const { return dim[0] == 0 || dim[1] == 0 || dim[2] == 0 || values.isEmpty(); }
    int cellCount() const { return int(dim[0]) * int(dim[1]) * int(dim[2]); }
    /// The signed distance stored for one cell, in mesh units. 0 for a cell out
    /// of range or a mesh with no field.
    float distanceAt(int x, int y, int z) const
    {
        if (isEmpty() || x < 0 || y < 0 || z < 0 || x >= int(dim[0]) || y >= int(dim[1]) ||
            z >= int(dim[2]))
            return 0.0f;
        const int i = (z * int(dim[1]) + y) * int(dim[0]) + x;
        if (i >= values.size()) return 0.0f;
        return float(static_cast<signed char>(values.at(i))) / 127.0f * scale;
    }
};

// CPU-side mesh: geometry buffers, skeleton, animations, bounds and the picking
// TriMesh. The GL half (VAO/draw) died with the legacy renderer at step 14; the
// engine mirror converts these buffers into engine meshes each time one changes.
class Mesh
{
    SkeletonPtr skeleton;
    QMap<QString, SkeletalAnimationPtr> skeletalAnimations;

	QList<VertexBufferPtr> vertexBuffers;
	IndexBufferPtr idxBuffer;

public:
    // Triangles because that is what every construction site sets it to
    // (meshbake.cpp:344, linemeshbuilder.cpp:28, gizmomeshes.cpp:97,
    // translationgizmo.cpp:495) and what the bake writes back out.
    PrimitiveMode primitiveMode = PrimitiveMode::Triangles;
    // `usesIndexBuffer` is written by both constructors and so satisfies
    // source.member_init without this — but Mesh(aiMesh*) writes it AFTER the
    // `if (!mesh->HasPositions()) return;` early return (mesh.cpp:89/181), which
    // is the exact shape the rule's `a helper can grow an early return` is about.
    bool usesIndexBuffer = false;

    BoundingSphere boundingSphere;
	AABB aabb;

    int numVerts;
    int numFaces;

    TriMesh* triMesh;

    /// ATOM stage 1 — the automatic LOD chain, built at IMPORT by MeshBake and
    /// carried in the .jmb bake (SPECS/NANITE_SPEC.md §7). Empty for every mesh
    /// that has none, which is today's behaviour exactly.
    ///
    /// `lodIndices[i]` is LEVEL i+1 and indexes the SAME vertex buffers as
    /// `idxBuffer` (level 0). meshopt_simplify REMOVES vertices, it never
    /// creates them, and nothing re-orders the vertex buffer per level on
    /// purpose: one vertex buffer, N index buffers is what keeps every level of
    /// a mesh inside ONE draw call downstream (finding B).
    ///
    /// `lodBounds[i]` is level i+1's MEASURED TWO-SIDED DISTANCE from level 0 —
    /// a sampled Hausdorff estimate with the sampling-gap margin applied — as a
    /// LENGTH in mesh units, monotonically non-decreasing. THIS is the currency
    /// the whole program is judged in and the only one any consumer reads:
    /// divided by the view distance it is a screen-space error, and compared
    /// against a world-space cell size it answers "is this level fine enough for
    /// a voxel of that size". The measurement is `meshbake.cpp`'s
    /// `lodchain::twoSidedDistance`; the one selection rule over it is
    /// `lodLevelForWorldError` (jahshaka/engine/Types.h).
    ///
    /// `lodErrors[i]` is what the SIMPLIFIER said about that level — its
    /// combined position+attribute quadric. It is a DIAGNOSTIC and nothing
    /// reads it to choose a level (ATOM P1's AT-A5): it is an estimate, not a
    /// bound, and the reasons are listed at the measurement in meshbake.cpp.
    /// Kept because "what the simplifier claimed" beside "what the surface
    /// measures" is how the margin and the knobs stay honest.
    ///
    /// Both arrays have one entry per level above 0 and the same length as
    /// `lodIndices`.
    QVector<QVector<quint32>> lodIndices;
    QVector<float>            lodErrors;
    QVector<float>            lodBounds;

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
    QVector<MeshCard> cards;

    /// The fraction of the mesh's sampled surfels covered by AT LEAST ONE card
    /// — the quality of the list above, measured by the generator against the
    /// real surface (occlusion included) and carried so a consumer never has to
    /// re-derive it. 0 when there are no cards.
    float cardCoverage = 0.0f;

    /// ATOM P2 / SUB-S5-SDF — the mesh's signed distance field, built at IMPORT
    /// by MeshBake beside the chain and the cards (see MeshSdf above). Empty for
    /// every mesh that gets none.
    MeshSdf sdf;

    /// CPU-side geometry, read-only. The engine mirror and importers convert from
    /// these.
    const QList<VertexBufferPtr>& getVertexBuffers() const { return vertexBuffers; }
    IndexBufferPtr getIndexBuffer() const { return idxBuffer; }

    TriMesh* getTriMesh()
    {
        return triMesh;
    }


    bool hasSkeleton();
    SkeletonPtr getSkeleton();

    void addSkeletalAnimation(QString name, SkeletalAnimationPtr anim);
    QMap<QString, SkeletalAnimationPtr> getSkeletalAnimations();
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
    static SkeletonPtr extractSkeleton(const aiMesh* mesh, const aiScene* scene);
    static QMap<QString, SkeletalAnimationPtr> extractAnimations(const aiScene *scene, QString source = "");



    //assumed ownership of vertexLayout
    static Mesh* create(void* data,int dataSize,int numElements,VertexLayout* vertexLayout);
	static MeshPtr create(VertexLayout vertexLayout);
	static MeshPtr create();

	Mesh();

    /// `withBones` false: the bone index/weight vertex arrays are NOT built,
    /// so a rigged file becomes static geometry. The import-settings switch
    /// `skeleton:false` (irisgl/import/importsettings.h §4.3) is the only
    /// caller that passes false; every other build is unchanged.
    Mesh(aiMesh* mesh, bool withBones = true);

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

private:
    void addVertexArray(VertexAttribUsage usage,void* data,int size,int type,int numComponents);

	void calculateBounds(const aiMesh* mesh);

    static BoundingSphere calculateBoundingSphere(const aiMesh* mesh);
};

}

#endif // MESH_H
