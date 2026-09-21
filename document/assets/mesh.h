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
        case 2: return Vec3(0, 0, 1);
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
    /// `lodErrors[i]` is that level's SIMPLIFIER error (position + attribute
    /// quadrics combined, >= the geometric error) as a LENGTH in mesh
    /// units, monotonically non-decreasing. It is the currency the whole
    /// program is judged in: divided by the view distance it is a screen-space
    /// error, and compared against a world-space cell size it answers "is this
    /// level fine enough for a voxel of that size".
    QVector<QVector<quint32>> lodIndices;
    QVector<float>            lodErrors;

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

    /// Loads (or ANSWERS FROM THE PARSE CACHE) the first mesh of a model file.
    ///
    /// Two nodes that name the same file get the SAME Mesh — the geometry is
    /// immutable after construction and node duplication has always shared a
    /// MeshPtr, so this is the model the document already had, applied to the
    /// path that was re-parsing a primitive's .obj on every add (ADD-1). The
    /// cache holds WEAK references: it never keeps a mesh alive, and a file
    /// whose last node is gone is parsed again next time.
    static MeshPtr loadMesh(QString filePath);
    /// PINS these paths: once parsed, a pinned model is held for the life of
    /// the process instead of being let go with its last node.
    ///
    /// The weak cache above is right for CONTENT — a hundred imported models
    /// must not be kept alive by a cache — and wrong for the handful of models
    /// that are compiled into the binary and reappear in every world the user
    /// opens. Those were re-parsed on the UI thread on every open, after every
    /// close dropped them: measured 1-4 parses and 17-95 ms per open of a
    /// shipped sample (OPEN-ASSIMP-1). The shell pins exactly the built-in
    /// primitives; nothing else in the tree calls this, and an unpinned path
    /// keeps the weak behaviour untouched.
    ///
    /// Registering a path does NOT parse it: the first real load does, and the
    /// strong reference is taken then. Safe from any thread, idempotent.
    static void pinLoadPaths(const QStringList &paths);
    /// Forgets every cached parse, pinned models included. For tests and for a
    /// tool that has just rewritten a model file on disk; nothing in the
    /// editor needs it. The pin REGISTRATIONS survive — they are a statement
    /// about which paths are shipped, not a cache.
    static void clearLoadCache();
    /// How many parses the cache is currently able to answer from.
    static int loadCacheSize();
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
