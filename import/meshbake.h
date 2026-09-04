/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef MESHBAKE_H
#define MESHBAKE_H

// MeshBake — a parsed model, frozen (MESH_BAKE_SPEC.md phase 1).
//
// THE PROBLEM. Opening a world re-PARSES every model it references with
// assimp: ~0.9 s for the Matcaps dragon, on the UI thread, every open,
// forever. MeshPrewarm hoisted that parse onto a worker; it did not remove
// it. Nothing in the tree ever cached a built form.
//
// THE BAKE is the built form: the exact `iris::Mesh` objects, skeletons,
// animation clips, per-material data and node hierarchy that
// `MeshNode::loadAsSceneFragment` / `GraphicsHelper::loadAllMeshesFromAssimpScene`
// produce from an aiScene, written once at IMPORT and read back at OPEN as a
// sequence of memcpys. It is DERIVED DATA — disposable, rebuildable, keyed on
// the source content plus the code that produced it; the original file stays
// in the CAS and stays the truth.
//
// WHY NOT Ogre's v2 `MeshSerializer` (the spec's first choice, rejected on
// evidence at the pinned Ogre-Next; recorded so nobody re-opens it blind):
//
//   1. `MeshSerializerImpl::importMesh` ends with
//      `if (!pMesh->hasValidShadowMappingVaos()) pMesh->prepareForShadowMapping(false)`
//      (OgreMesh2SerializerImpl.cpp:149-150) — the format does not carry the
//      shadow-caster VAO, so every LOAD rebuilds it through
//      `VertexShadowMapHelper::shrinkVertexBuffer`'s nested-loop memcmp over
//      every vertex pair (OgreVertexShadowMapHelper.cpp:302-315). That is the
//      O(n^2) pass this codebase measured at 10.2 SECONDS on the Matcaps
//      dragon and replaced with a hash in OgreMesh.cpp's buildShadowVao. A
//      .mesh bake would make opens slower, not faster.
//   2. Export is NOT readback-free: `BufferPacked::readRequest`
//      (OgreBufferPacked.cpp:118-124) always goes through a StagingBuffer
//      async download; it has no shadow-copy short-circuit.
//   3. Both directions need a live `VaoManager` (the `MeshSerializer` ctor),
//      i.e. the render thread — while `MeshImporter::convert` runs on the
//      import worker.
//
// The bake therefore lives on the DOCUMENT side, is pure CPU, and is engine
// independent by construction: nothing an Ogre rebuild can change is in it.
//
// FINGERPRINT. `<format>|<producer>|<assimp>|<flags>|<sourceOid>` — a bake
// whose fingerprint does not match what this build would produce is ignored
// and rebuilt, exactly like the shader cache. `producer` is a compile-time
// hash of the TUs that BUILD the bake (JAHSHAKA_MESH_BAKE_PRODUCER_ID, set by
// irisgl/CMakeLists.txt with the same file(SHA256) mechanism the engine's
// shader-cache fingerprint uses).
//
// THREADING: everything here is plain data. build/serialize run on the import
// worker; read/deserialize run on the open worker; buildFragment runs on
// whichever thread consumes it.

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QQuaternion>
#include <QString>
#include <QVector>
#include <QVector3D>
#include <functional>

#include "irisglfwd.h"
#include "document/assets/mesh.h"   // MeshMaterialData

class aiScene;

namespace iris
{

/// One node of the baked fragment hierarchy — the shape
/// `MeshNode::loadAsSceneFragment` builds, without the aiScene.
struct BakedNode
{
    QString name;
    QVector3D pos;
    QVector3D scale = QVector3D(1, 1, 1);
    QQuaternion rot;
    /// True when this node is a MeshNode (carries geometry).
    bool isMeshNode = false;
    /// Index into Model::meshes (aiScene mesh order), or -1.
    int meshIndex = -1;
    /// Index into Model::materials, or -1.
    int materialIndex = -1;
    QVector<BakedNode> children;
    /// How many LEADING children are the synthesized mesh children an aiNode
    /// with several meshes produces. `_buildScene` treats those differently
    /// from recursion children — no rootBone, `addChild` with the default
    /// keepTransform — so the distinction has to survive the bake.
    int meshChildCount = 0;
};

class MeshBake
{
public:
    /// The whole baked model: everything two open-path consumers need.
    struct Model
    {
        bool valid = false;
        QString fingerprint;
        /// aiScene mesh order — `meshIndex` on a MeshNode indexes this.
        QList<MeshPtr> meshes;
        QMap<QString, SkeletalAnimationPtr> animations;
        QVector<MeshMaterialData> materials;
        /// The fragment tree. `singleMesh` mirrors loadAsSceneFragment's
        /// single-mesh shortcut (one MeshNode, no children).
        BakedNode root;
        bool singleMesh = false;
    };

    /// The format this build writes and reads.
    static int formatVersion();

    /// `<format>|<producer>|<assimp>|<flags>` — everything except the content.
    static QString producerId();

    /// The full key for a source whose content id is `sourceOid`. Empty
    /// `sourceOid` yields a fingerprint that can never match a stored bake.
    static QString fingerprintFor(const QString &sourceOid);

    /// The canonical bake file name for a source content id.
    static QString fileNameFor(const QString &sourceOid);

    /// The role bakes are recorded under in the CAS (`asset_files.role`).
    static QString casRole();

    /// Build the bake from an ALREADY PARSED scene (the import side pays no
    /// second parse). `extractDir` is handed to MaterialHelper exactly as
    /// loadAsSceneFragment would.
    static Model buildFromScene(const aiScene *scene, const QString &filePath,
                                const QString &fingerprint,
                                const QString &extractDir = QString());

    /// Parse `filePath` and bake it. Used by the lazy re-bake of an existing
    /// library, where no parse is in flight.
    static Model buildFromFile(const QString &filePath, const QString &fingerprint,
                               const QString &extractDir = QString());

    /// Deterministic: the same Model always serializes to the same bytes
    /// (assets.checkConsistency re-derives the object set and compares oids).
    static QByteArray serialize(const Model &model);

    /// Never throws, never half-builds: a short, truncated, corrupt,
    /// wrong-version or wrong-fingerprint blob returns `valid == false` and
    /// the caller falls back to the parse path.
    static Model deserialize(const QByteArray &blob,
                             const QString &expectFingerprint = QString());

    /// Read + deserialize. Missing file = invalid Model, no warning.
    static Model read(const QString &path,
                      const QString &expectFingerprint = QString());

    /// Write ATOMICALLY (temp + rename in the same directory): a bake at its
    /// final path is either absent or complete, even through a SIGKILL.
    static bool write(const QString &path, const Model &model, QString *errorOut);

    /// The baked equivalent of `MeshNode::loadAsSceneFragment` — same node
    /// shape, same materials, same animations, no assimp.
    static SceneNodePtr buildFragment(
        const Model &model, const QString &filePath,
        const std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData &data)> &createMaterialFunc);
};

}   // namespace iris

#endif   // MESHBAKE_H
