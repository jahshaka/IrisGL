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
// FINGERPRINT. `<format>|<producer>|<assimp>|<flags>|<sourceOid>|<settings>` —
// a bake whose fingerprint does not match what this build would produce is
// ignored and rebuilt, exactly like the shader cache. The SETTINGS term
// (IMPORT-1, import/importsettings.h) is what makes two imports of one source
// file under different import settings two different bakes: since the import
// dialog an asset's scale, rotation and origin are BAKED, so the same bytes
// legitimately produce different geometry.
//
// THE KEY HAS THREE PARTS, and each answers a different question (BAKEKEY-1,
// 2026-09-15; irisgl/CMakeLists.txt carries the measurement and the per-file
// justification, MESH_BAKE_SPEC.md the decision):
//
//   `format`   = kFormatVersion in meshbake.cpp, HAND-BUMPED. It covers the
//                on-disk layout AND the document-side meaning of what is
//                serialized — the vertex layout, the skeleton, the material
//                fields and the colour space they are recorded in. The classes
//                that produce those bytes (document/assets/mesh.*,
//                document/assets/skeleton.*, document/scenegraph/meshnode.cpp,
//                core/geometry/trimesh.cpp) are covered HERE, by hand, because
//                they are edited constantly for reasons that cannot move a
//                baked byte: hashing them threw every library's bakes away
//                about once a day (25 of 408 irisgl commits, 10 of 11 working
//                days). tests/hygiene/bake_key_guard.sh (ctest:
//                source.bake_key_guard) is the gate that keeps the hand bump
//                from being forgotten — it fails on a commit touching one of
//                those files unless the version moved in the same range or the
//                commit message carries the line `bake-output: unchanged`.
//   `producer` = JAHSHAKA_MESH_BAKE_PRODUCER_ID, a configure-time SHA256 over
//                the THREE files that WRITE a bake and whose text change means
//                the produced bytes can change: import/meshbake.cpp (the
//                builder and the serializer), import/meshbake.h (the
//                declarations the serializer walks) and
//                import/materialhelper.cpp (extractMaterialData fills every
//                material record). Same file(SHA256) mechanism the engine's
//                shader-cache fingerprint uses; `hashedSources()` reports the
//                list this build was compiled with.
//   `assimp` / `flags` = the importer version and the EXACT value of
//                ImportFlags::Canonical — which is why the importflags files
//                themselves are not hashed (the integer is more precise than a
//                hash of the prose around it).
//
// THREADING: everything here is plain data. build/serialize run on the import
// worker; read/deserialize run on the open worker; buildFragment runs on
// whichever thread consumes it.

#include "core/math/quat.h"
#include "core/math/vec.h"
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "irisglfwd.h"
#include "import/scenesource.h"
#include "document/assets/mesh.h"   // MeshMaterialData

struct aiScene;

namespace iris
{

/// One node of the baked fragment hierarchy — the shape
/// `MeshNode::loadAsSceneFragment` builds, without the aiScene.
struct BakedNode
{
    QString name;
    iris::Vec3 pos;
    iris::Vec3 scale = iris::Vec3(1, 1, 1);
    iris::Quat rot;
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

    /// The producer HASH term alone, as this build was compiled with it
    /// ("dev" when CMake supplied none).
    static QString producerHash();

    /// The repo-root-relative files whose text is hashed into that term — the
    /// list irisgl/CMakeLists.txt hashes, handed to the compiler so the suite
    /// can prove the two agree (and that the document-side classes are out of
    /// it). Empty in a build without the define.
    static QStringList hashedSources();

    /// The producer hash for an arbitrary ordered list of files, by the SAME
    /// algorithm irisgl/CMakeLists.txt uses. Exists so the key's definition is
    /// TESTABLE without rebuilding: the suite checks it reproduces the
    /// compiled-in term for the real sources, then varies scratch copies.
    /// Empty when any file cannot be read.
    static QString producerHashOf(const QStringList &absolutePaths);

    /// The full key for a source whose content id is `sourceOid`, imported
    /// under the import settings whose hash is `settingsHash`
    /// (import/importsettings.h). Empty `sourceOid` yields a fingerprint that
    /// can never match a stored bake; an EMPTY `settingsHash` means IDENTITY
    /// settings — what every row imported before the import dialog, every
    /// shipped sample and every `.jaf` archive carries — and keys exactly as a
    /// fully-defaulted record does.
    static QString fingerprintFor(const QString &sourceOid,
                                  const QString &settingsHash = QString());

    /// The canonical bake file name, `<oid16>-<settingsHash>.jmb`. The settings
    /// term is part of the NAME and not only of the fingerprint because the
    /// catalog lookup is BY NAME (services/meshbakestore.cpp): two imports of
    /// one source under different settings are two bakes that must coexist,
    /// and a shared name would make the newest one shadow the other forever.
    static QString fileNameFor(const QString &sourceOid,
                               const QString &settingsHash = QString());

    /// The role bakes are recorded under in the CAS (`asset_files.role`).
    static QString casRole();

    /// ATOM stage 1 (SPECS/NANITE_SPEC.md §7): build `mesh`'s automatic LOD
    /// chain, in place — `lodIndices` + `lodErrors`, or both cleared when the
    /// mesh gets no chain (it is skinned, it is not triangles, it is too small
    /// to be worth a level, or its topology stopped the simplifier early).
    ///
    /// buildFromScene calls this for every mesh it bakes; it is public because
    /// the chain is a PRODUCT of the bake with its own policy, and a caller that
    /// builds an iris::Mesh by other means (a procedural mesh, a benchmark
    /// fixture, a re-bake path) must be able to ask for the same levels the
    /// importer would have produced rather than a second implementation of them.
    /// Pure CPU, no assimp, no engine; safe from any thread.
    static void buildLodChain(const MeshPtr &mesh);

    /// SURFACE-CACHE phase 1 (SPECS/SURFACE_CACHE_ASSESSMENT.md §7): build
    /// `mesh`'s SURFACE CARD LIST, in place — `cards` + `cardCoverage`, or both
    /// cleared when the mesh gets none (it is skinned, it is not triangles, it
    /// has no area, or `maxCards` is zero).
    ///
    /// MUST RUN AFTER buildLodChain on the same mesh: a card records the LOD
    /// level its own texel picks, and a mesh with no chain yet would have every
    /// card name level 0.
    ///
    /// `maxCards` is the budget (ImportTransform::maxCards, default 12 — Epic's
    /// "Max Lumen Mesh Cards"); it is clamped to the format's ceiling of 64.
    /// Public for the same reason buildLodChain is: the card list is a PRODUCT
    /// of the bake with its own policy, and a caller that builds an iris::Mesh
    /// by other means — a document PRIMITIVE, which never goes through an
    /// import (Mesh::loadMesh), a procedural mesh, a re-bake path — must be
    /// able to ask for the cards the importer would have produced rather than
    /// grow a second implementation of them.
    /// Pure CPU, no assimp, no engine; safe from any thread.
    static void buildCards(const MeshPtr &mesh, int maxCards);

    /// ATOM P2 / SUB-S5-SDF: build `mesh`'s SIGNED DISTANCE FIELD, in place —
    /// `sdf`, or an empty field when the mesh gets none (it is skinned, it is not
    /// triangles, or it has no extent).
    ///
    /// MUST RUN AFTER buildLodChain on the same mesh: the field's cell size is
    /// floored at four times LEVEL 1's measured bound, so that the field is never
    /// finer than the geometry is honest (document/assets/mesh.h MeshSdf). A mesh
    /// with no chain gets the resolution ceiling instead, which is correct — a
    /// mesh the simplifier could not touch has no coarser truth to respect.
    ///
    /// Public for the same reason buildLodChain and buildCards are: it is a
    /// PRODUCT of the bake with its own policy, and a caller that builds an
    /// iris::Mesh by other means must be able to ask for the field the importer
    /// would have produced rather than grow a second implementation of it.
    /// Pure CPU, no assimp, no engine; safe from any thread.
    static void buildSdf(const MeshPtr &mesh);

    /// VERIFICATION, and it exists for the same reason `producerHashOf` does: the
    /// claim has to be TESTABLE without re-running the thing that made it.
    ///
    /// `checkLodBounds` re-measures every level of `mesh` against level 0 by AREA
    /// SAMPLING ALONE, with `densityMultiple` times as many samples as the bake
    /// used, and answers whether every measured distance is inside the stored
    /// `lodBounds[k]`.
    ///
    /// WHAT IT IS AND IS NOT. It is not an independent derivation of the bound: the
    /// bake's maximum comes from the REMOVED BASE VERTICES, computed exactly, and
    /// any check that walked them too would reproduce that term bit for bit and
    /// pass by construction. Dropping them is what leaves the sampling-gap margin
    /// exposed — so this is a REGRESSION CHECK on the margin and on the whole
    /// pipeline (sampler, grid, floor, monotonicity), at a sample count and a set
    /// of strata the bake never used. A mesh with no chain trivially passes.
    /// `worstRatioOut` receives the largest (dense measurement / stored bound) seen,
    /// so a failure reports a number instead of a boolean.
    static bool checkLodBounds(const MeshPtr &mesh, int densityMultiple = 8,
                               double *worstRatioOut = nullptr);

    /// ...and whether `mesh`'s SDF agrees with LEVEL 0's surface. Walks the cells
    /// inside the field's exact band, compares each stored distance with the exact
    /// nearest-surface distance, and answers whether the worst disagreement is
    /// under ONE CELL. `worstCellsOut` receives that worst disagreement in cells
    /// and `probedOut` how many cells were in the band (a field whose band is
    /// empty has proven nothing, which is why the suite asserts the count too).
    static bool checkSdfAgainstSurface(const MeshPtr &mesh, double *worstCellsOut = nullptr,
                                       int *probedOut = nullptr);

    /// ...and whether every cell the GEOMETRY says is OUTSIDE has a POSITIVE stored
    /// distance. A separate check because `checkSdfAgainstSurface` compares
    /// MAGNITUDES and is therefore blind to the one defect the field's normal can
    /// have: at a convex feature sharper than a right angle the nearest point of an
    /// exterior cell lies ON that feature, and a single face normal there can face
    /// away from the cell — so the cell reads inside. Inside/outside is decided here
    /// WITHOUT the field, by ray parity along +X through level 0's triangles, so
    /// the field is judged against the geometry and not against itself. Cells the
    /// parity test cannot answer (a ray grazing an edge) are skipped, as are cells
    /// within a cell of the surface, where the true sign is genuinely ambiguous.
    static bool checkSdfExteriorSign(const MeshPtr &mesh, int *probedOut = nullptr,
                                     int *wrongOut = nullptr);

    /// The capture resolution a baked card's LOD level was chosen for (Lumen's
    /// 128-texel page). Phase 2's atlas owns the page size it actually
    /// allocates; this is the number the BAKE assumed, so the two can be
    /// compared instead of silently disagreeing.
    static int cardCaptureResolution();

    /// Build the bake from an ALREADY PARSED scene (the import side pays no
    /// second parse). `extractDir` is handed to MaterialHelper exactly as
    /// loadAsSceneFragment would.
    /// `xf` is the asset's resolved import recipe (import/importsettings.h):
    /// the GEOMETRY half was already applied when `source` was parsed, and the
    /// TUNING half (skeleton / clips / materials) is applied HERE, to what is
    /// built out of it.
    static Model buildFromScene(const SceneSource &source, const QString &filePath,
                                const QString &fingerprint,
                                const QString &extractDir = QString(),
                                const ImportTransform &xf = ImportTransform());

    /// IrisGL-internal form (a complete aiScene needs assimp's headers).
    static Model buildFromScene(const aiScene *scene, const QString &filePath,
                                const QString &fingerprint,
                                const QString &extractDir = QString(),
                                const ImportTransform &xf = ImportTransform());

    /// Parse `filePath` and bake it. Used by the lazy re-bake of an existing
    /// library, where no parse is in flight.
    static Model buildFromFile(const QString &filePath, const QString &fingerprint,
                               const QString &extractDir = QString(),
                               const ImportTransform &xf = ImportTransform());

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

    /// HEADER-ONLY probe: magic + version + fingerprint, no payload read.
    /// The catalog can hold several producer GENERATIONS of one model under
    /// the same bake name (the name derives from the source oid alone) — a
    /// lookup walking candidates needs a cheap "is this row the current
    /// generation" test, not a full deserialize per stale row.
    static bool headerMatches(const QString &path, const QString &expectFingerprint);

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
