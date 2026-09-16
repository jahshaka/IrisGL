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
#include "import/importsettings.h"

namespace iris
{

class SceneSource;

/// WHAT A LIGHT PRE-READ OF A MODEL FILE TELLS THE IMPORT DIALOG
/// (SPECS/IMPORT_DIALOG_SPEC.md §8).
///
/// The dialog has to show a person the size, the unit, the clip list and
/// whether the file is rigged BEFORE anything is imported — and it has to do
/// it fast enough to sit in front of a drop. So this is a parse with NO
/// post-processing at all (the ~3x cheaper half, import/importflags.h): no
/// triangulation, no tangents, and — the part that matters here — NO
/// aiProcess_GlobalScale, so the vertices are still in the FILE'S OWN UNITS.
///
/// That is deliberate, not a limitation: the dialog's whole job is to let a
/// person disagree with the file about what a unit is, and the preview it
/// draws is
///
///     metres = source units x (the unit in force) x (the user's scale)
///
/// where "the unit in force" is `declaredUnitScale` under `units: "auto"` and
/// the user's override otherwise (import/importsettings.h). A pre-read that
/// had already applied the declaration could not show the difference.
struct ModelPreRead
{
    bool parsed = false;        ///< false = `error` says why, everything else is empty
    QString error;

    /// Metres per source unit AS THE FILE DECLARES IT (FBX UnitScaleFactor/100;
    /// 1 for every format at this pin that declares nothing).
    double declaredUnitScale = 1.0;

    /// The model's axis-aligned bounding box IN SOURCE UNITS — mesh-local
    /// boxes pushed through each instancing node's transform, exactly as
    /// ModelSceneInfo's extent is measured. Both corners, not just the size:
    /// the dialog's origin helpers (Centre, Bottom-centre) need to know WHERE
    /// the model sits, not only how big it is.
    double aabbMin[3] = { 0.0, 0.0, 0.0 };
    double aabbMax[3] = { 0.0, 0.0, 0.0 };
    bool aabbValid = false;

    int meshes = 0;
    int materials = 0;
    /// Distinct bone names across the meshes. `rigged` is the dialog's
    /// "this is a character" test — the only thing the suggestion line needs.
    int bones = 0;
    bool rigged() const { return bones > 0; }

    /// The clip names THIS FILE WOULD IMPORT AS, in file order, produced by
    /// the one naming rule (import/clipnaming.h) — so the dialog's checklist
    /// offers exactly the names the `clips` setting filters on.
    QStringList clipNames;

    /// Parse `filePath` with no post-processing and describe it. Goes through
    /// the choke point (import/scenesource.h) with an identity transform, like
    /// every other parse in this library.
    ///
    /// `formatHint` (an extension without the dot) is for a read straight off
    /// the content-addressed store, whose objects are named by their hash and
    /// carry no extension to dispatch on — the reimport dialog's case.
    static ModelPreRead read(const QString &filePath, const QString &formatHint = QString());
};

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
    /// as Studio's document-side extent::measureNode, so the number recorded
    /// at import and the number a placed node measures agree). `extentValid`
    /// is false for a scene with no geometry or a degenerate one.
    double extentX = 0.0;
    double extentY = 0.0;
    double extentZ = 0.0;
    bool extentValid = false;

    /// Metres per source unit AS THE FILE DECLARED IT (FBX's
    /// GlobalSettings::UnitScaleFactor, centimetres per unit — a Mixamo
    /// download says 1.0); 1.0 for every format that declares nothing.
    /// Recorded for the user — the import dialog shows it so a person can
    /// disagree with the file — and for the choke point, which needs it to
    /// resolve a UNIT OVERRIDE without a probe parse (import/scenesource.h).
    double declaredUnitScale = 1.0;

    /// Facts of a scene an import already parsed (no second parse).
    static ModelSceneInfo fromSource(const SceneSource &source);

    /// Parse `filePath` with the canonical preset and describe it. `parsed`
    /// is false when the importer refused the file.
    ///
    /// `xf` is the ASSET's import recipe (import/importsettings.h). It matters:
    /// the `extent` this records is what the asset MEASURES, and a describe
    /// that parsed with identity would report the file's authored size for an
    /// asset the import scaled (the second read's F7).
    static ModelSceneInfo read(const QString &filePath,
                               const ImportTransform &xf = ImportTransform());

    /// The file's OWN unit declaration alone, from a LIGHT parse (no
    /// post-processing): metres per source unit, 1.0 for a format that
    /// declares nothing and for a file that cannot be read. The one caller is
    /// readSceneFile resolving a unit OVERRIDE, which needs the declaration
    /// before it can hand assimp a scale (import/scenesource.h).
    static double readDeclaredUnitScale(const QString &filePath);

    /// The version of the import library these facts come from ("6.0.<rev>"
    /// at the pin), for the import determinism record (ASSET_PIPELINE_SPEC
    /// §3.2.2): same content + same settings + same importer version = same
    /// objects. Also a term of the mesh bake's producer id.
    static QString importerVersion();
};

} // namespace iris

#endif // IRIS_MODELSCENEINFO_H
