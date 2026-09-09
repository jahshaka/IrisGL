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
#include <QColor>

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
    float shininess;

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
    PrimitiveMode primitiveMode;
    bool usesIndexBuffer;

    BoundingSphere boundingSphere;
	AABB aabb;

    VertexLayout* vertexLayout;
    int numVerts;
    int numFaces;

    TriMesh* triMesh;
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

    static MeshPtr loadMesh(QString filePath);
    static MeshPtr loadAnimatedMesh(QString filePath);
    static SkeletonPtr extractSkeleton(const aiMesh* mesh, const aiScene* scene);
    static QMap<QString, SkeletalAnimationPtr> extractAnimations(const aiScene *scene, QString source = "");



    //assumed ownership of vertexLayout
    static Mesh* create(void* data,int dataSize,int numElements,VertexLayout* vertexLayout);
	static MeshPtr create(VertexLayout vertexLayout);
	static MeshPtr create();

	Mesh();

    Mesh(aiMesh* mesh);

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
