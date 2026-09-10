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

    /// Parses `filePath` with the canonical preset (ImportFlags::Canonical).
    /// False, with errorString() set, when the file could not be read; a
    /// second read releases the first scene.
    bool read(const QString &filePath);

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

} // namespace iris

#endif // IRIS_SCENESOURCE_H
