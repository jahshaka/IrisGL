/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_SCENEBUILDER_H
#define IRIS_SCENEBUILDER_H

// THE IMPORT SIDE OF THE DOCUMENT (NOQT-DOCUMENT-1 stage 1): what BUILDS document objects from an assimp parse —
// the importer's material record, the fragment loaders, the aiMesh -> Mesh build, the skeleton and clip extraction.
// Moved verbatim out of document/assets/mesh.{h,cpp} and document/scenegraph/meshnode.{h,cpp} so the document
// includes neither assimp nor import/ (tests/hygiene/assimp_import_only.sh). A BAKE PRODUCER: MeshBake builds every
// baked mesh, rig, clip and material record through it, so both files are in the HASHED half of the bake key
// (irisgl/CMakeLists.txt _mesh_bake_hashed_sources, DOCS/traps/engine/build-media-and-bake.md "THE BAKE KEY").
// No assimp in this header: assimp is a PRIVATE dependency of IrisGL — the two scene types are forward-declared and
// the parse a caller may keep is the opaque SceneSource.

#include <QColor>
#include <QMap>
#include <QString>
#include <functional>

#include "irisglfwd.h"
#include "import/scenesource.h"

struct aiScene;
struct aiMesh;

namespace iris
{

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
    /// iris::materialFromMeshData (import/meshmaterial.h) for what carries the colour there.
    bool unlit = false;
    /// glTF `doubleSided` (and every other format's two-sided flag), as assimp's
    /// AI_MATKEY_TWOSIDED (CULL-MODE-1): the import writes it onto the MeshNode
    /// as faceCullingMode None — the node's cull is what the mirror pushes.
    bool twoSided = false;
    /// HOW THE SURFACE COVERS (BISTRO-IMPORT-1) — PbrMaterial's `alphaMode` row
    /// (0 opaque, 1 masked, 3 glass) and its `alpha` / `alphaCutoff`. Read from the
    /// SOURCE's own facts by MaterialHelper::extractMaterialData: an FBX/OBJ opacity
    /// below 1 is glass at that alpha; a bound base-colour map whose own alpha channel
    /// has texels below the cutoff is a cutout. Every import used to drop both.
    int alphaMode = 0;
    float alpha = 1.0f;
    float alphaCutoff = 0.5f;
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

class IModelReadProgress
{
public:
    virtual float onProgress(float percentage) = 0;
};

/**
 * Some model contains multiple meshes with child-parent relationships. This funtion Loads the model as a scene
 * itself. If only one mesh is in the scene, it returns it as an MeshNode rather than a SceneNode. Otherwise, it
 * returns a null shared pointer.
 * @param path
 * @return
 */
/// `scene_` may be null: a local importer is used and the aiScene dies
/// with the call (everything returned owns copies). Pass a SceneSource
/// only to keep the aiScene alive for follow-up work (metadata counts).
/// `extractDir`: where embedded textures and derived maps are WRITTEN
/// (import staging). Empty = beside the source file (legacy behavior —
/// wrong for read-only sources; the import pipeline always passes one).
/// `xf`: the ASSET's resolved import recipe (import/importsettings.h) —
/// the scale, rotation and origin its settings baked in, AND the tuning
/// half (skeleton / clips / materials) that decides what is built from the
/// parse. Identity for a raw path with no library row behind it; a library
/// asset's parse that passes identity renders a DIFFERENT SIZE, or a
/// different set of products, from the asset's own bake.
SceneNodePtr loadAsSceneFragment(
    QString path,
    std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
    SceneSource *scene_ = Q_NULLPTR,
    IModelReadProgress* progressReader = Q_NULLPTR,
    const QString &extractDir = QString(),
    const ImportTransform &xf = ImportTransform()
);

/// IrisGL-internal form of the above (a complete aiScene needs assimp's
/// headers, which only this library's own translation units and the
/// white-box importer suites include).
SceneNodePtr loadAsSceneFragment(
	const QString &filePath,
	const aiScene* scene_,
	std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
	const QString &extractDir = QString(),
	const ImportTransform &xf = ImportTransform()
);

/// The aiMesh -> Mesh build (vertex arrays, indices, picking TriMesh, bounds). `withBones` false: the bone
/// index/weight vertex arrays are NOT built, so a rigged file becomes static geometry. The import-settings switch
/// `skeleton:false` (irisgl/import/importsettings.h §4.3) is the only caller that passes false; every other build
/// is unchanged.
MeshPtr meshFromAiMesh(aiMesh* mesh, bool withBones = true);
SkeletonPtr extractSkeleton(const aiMesh* mesh, const aiScene* scene);
QMap<QString, SkeletalAnimationPtr> extractAnimations(const aiScene *scene, QString source = "");

}

#endif // IRIS_SCENEBUILDER_H
