/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_SCENESOURCE_H
#define IRIS_SCENESOURCE_H

#include <QString>
#include <memory>

#include "import/importsettings.h"

namespace Assimp { class Importer; }
struct aiScene;

namespace iris
{

/// ONE parsed model file, owned. The parsed scene lives exactly as long as
/// this object does, so a caller that needs the parse to outlive the call
/// that made it (the import: fragment build, then metadata, then the mesh
/// bake — all from ONE parse) passes its own SceneSource and keeps it.
///
/// OPAQUE outside IrisGL (ENGINEERING_DEBT L4 part 3): the importer and the
/// scene it holds are assimp types, and assimp is a PRIVATE dependency of
/// this library — Studio's include path does not carry its headers. What
/// Studio can do with one is read() it, ask whether it hasScene(), and hand
/// it to the IrisGL entry points that consume one (MeshNode::loadAsSceneFragment,
/// GraphicsHelper::loadAllMeshesAndAnimationsFromSource, ModelSceneInfo::fromSource,
/// MeshBake::buildFromScene). The two accessors at the bottom return
/// forward-declared assimp types and are usable only by a translation unit
/// that includes assimp itself, i.e. IrisGL's own.
///
/// THREADS: assimp is thread-safe only across INDEPENDENT importers, which is
/// exactly what each SceneSource owns — one per parse, never shared between
/// threads while a read is in flight.
class SceneSource
{
public:
    SceneSource();
    ~SceneSource();
    SceneSource(const SceneSource &) = delete;
    SceneSource &operator=(const SceneSource &) = delete;

    /// Parses `filePath` with the canonical preset (ImportFlags::Canonical) —
    /// a file on disk, or a Qt resource (":/..." or "qrc:/...") through
    /// readSceneFile below. False, with errorString() set, when the file could
    /// not be read; a second read releases the first scene.
    ///
    /// `xf` is the ASSET'S IMPORT TRANSFORM (import/importsettings.h): the
    /// scale, rotation and origin its import settings baked into it. Default =
    /// identity, which is what a raw path with no library row behind it gets.
    bool read(const QString &filePath, const ImportTransform &xf = ImportTransform());

    /// True while a successfully parsed scene is held.
    bool hasScene() const;

    /// assimp's error text after a failed read (empty after a successful one).
    QString errorString() const;

    // ---- IrisGL-internal (complete types need assimp's headers) ------------
    const aiScene *scene() const;
    Assimp::Importer &importer();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

/// THE ONE assimp READ of a model path, for every IrisGL caller that holds its
/// own importer (IrisGL-internal: the importer is an assimp type).
///
/// A Qt RESOURCE (":/..." or "qrc:/...") cannot be opened by assimp, so it is
/// read into memory and handed over WITH ITS EXTENSION as the format hint.
/// Without the hint assimp names the buffer "$$$___magic___$$$." — no
/// extension — and falls back to SNIFFING only the first 200 bytes for a
/// format keyword (BaseImporter::SearchFileHeaderForToken's default window),
/// so a comment block above an OBJ's `mtllib` made the default scene's ground
/// unloadable with nothing but "error parsing file" (smoke L10 item 4). With
/// the hint the resource is dispatched by extension, exactly as a file on disk
/// always was. Returns null (the importer holds the error) on failure.
///
/// THE CHOKE POINT for the import transform (SPECS/IMPORT_DIALOG_SPEC.md §4.1).
/// Every canonical parse in this tree goes through here, and `xf` is how an
/// asset's baked scale/rotation/origin reaches it: the scale as assimp's own
/// GLOBAL_SCALE_FACTOR before the read, the rotation and the metre-valued
/// translation as a pre-multiply of the scene ROOT after it. A parse that
/// skipped it would render a DIFFERENT SIZE from the bake — the exact class
/// services/meshbakestore.cpp already documents for the unit factor — which is
/// why tests/hygiene/one_readfile.sh refuses any other ReadFile call site.
const aiScene *readSceneFile(Assimp::Importer &importer, const QString &filePath,
                             unsigned int flags,
                             const ImportTransform &xf = ImportTransform());

} // namespace iris

#endif // IRIS_SCENESOURCE_H
