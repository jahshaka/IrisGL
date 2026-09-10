/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_MODELSCENEINFO_H
#define IRIS_MODELSCENEINFO_H

#include <QString>
#include <QStringList>
#include <QVector>

namespace iris
{

class SceneSource;

/// The FACTS of a parsed model file, as plain data — what Studio's asset
/// metadata block (services/assetmetadata.cpp, `kind: "model"`) is computed
/// from, read off the parse once and handed over with no importer type in
/// sight (ENGINEERING_DEBT L4 part 3: assimp is IrisGL-private). Every
/// number here is measured on the CANONICAL parse (ImportFlags::Canonical —
/// triangulated, unit-converted), so the counts match the geometry every
/// load produces.
///
/// The policy that turns these into the block — which channels are pivot
/// bookkeeping, the rig id hash, the fit-to-size inference — is Studio's and
/// stays there; this struct carries names and numbers only.
struct ModelSceneInfo
{
    bool parsed = false;        ///< the file was read (false = error set, everything else empty)
    QString error;              ///< the importer's message when it was not

    qint64 vertices = 0;        ///< summed over the meshes
    qint64 triangles = 0;       ///< faces summed over the meshes (the canonical parse triangulates)
    int meshes = 0;
    int materials = 0;

    /// Distinct texture references across every material and slot type, in
    /// first-seen order. Embedded textures ("*0" paths) are references too, so
    /// a purely embedded model still counts them.
    QStringList textureReferences;
    /// The scene's embedded texture count (what a model with no material
    /// references at all still carries).
    int embeddedTextures = 0;

    /// Bone names from the meshes' bone lists, first-seen order, de-duplicated,
    /// empties skipped.
    QStringList boneNames;
    /// Scene-node names, pre-order, de-duplicated, empties skipped. Both lists
    /// matter: the clip <-> rig join is by SCENE-NODE name, and an FBX carries
    /// bones the importer never gives a mesh.
    QStringList nodeNames;

    struct Animation
    {
        QString name;                   ///< as the file names it (may be empty)
        double ticksPerSecond = 0.0;    ///< raw; 0 in more exports than not
        double durationTicks = 0.0;     ///< raw
        /// durationTicks / ticksPerSecond, with the importer's documented
        /// 25 fps fallback when the file states none — the same rule
        /// Mesh::extractAnimations keys its tracks with.
        double lengthSeconds = 0.0;
        QStringList channelNames;       ///< one per channel, the node it drives
    };
    QVector<Animation> animations;

    /// The model's axis-aligned WORLD size in metres: mesh-local AABBs pushed
    /// through each instancing node's accumulated transform (the same shape
    /// as Studio's document-side fitsize::measureNode, so the number recorded
    /// at import and the number a placed node measures agree). `extentValid`
    /// is false for a scene with no geometry or a degenerate one.
    double extentX = 0.0;
    double extentY = 0.0;
    double extentZ = 0.0;
    bool extentValid = false;

    /// Metres per source unit AS THE FILE DECLARED IT (FBX's
    /// GlobalSettings::UnitScaleFactor, centimetres per unit — a Mixamo
    /// download says 1.0); 1.0 for every format that declares nothing.
    /// Recorded for the user: the canonical parse has already applied it.
    double declaredUnitScale = 1.0;

    /// Facts of a scene an import already parsed (no second parse).
    static ModelSceneInfo fromSource(const SceneSource &source);

    /// Parse `filePath` with the canonical preset and describe it. `parsed`
    /// is false when the importer refused the file.
    static ModelSceneInfo read(const QString &filePath);

    /// The version of the import library these facts come from ("6.0.<rev>"
    /// at the pin), for the import determinism record (ASSET_PIPELINE_SPEC
    /// §3.2.2): same content + same settings + same importer version = same
    /// objects. Also a term of the mesh bake's producer id.
    static QString importerVersion();
};

} // namespace iris

#endif // IRIS_MODELSCENEINFO_H
