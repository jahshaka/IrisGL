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
#include <atomic>
#include <functional>
#include <memory>

#include "irisglfwd.h"
#include "import/clipfileinfo.h"
#include "import/modelsceneinfo.h"
#include "import/scenesource.h"
#include "document/assets/mesh.h"   // MeshMaterialData
#include "import/atomdag/build.h"   // ClusterDagStats / Options

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
        /// THE FACTS OF THE PARSE THE BAKE WAS BUILT FROM (SHIPPED-BAKES-1):
        /// counts, texture references, bone and node names, the clip table, the
        /// measured extent and the file's declared unit — exactly what
        /// ModelSceneInfo reads off the same aiScene. Studio's metadata backfill
        /// reads it from here (services/assetmetadata.cpp), so describing a
        /// library model after its import is a bake read, never a parse.
        ModelSceneInfo describe;
        /// WHAT THE BAKE COST, per mesh and per stage (IMPORT-SPEED-1): the import's
        /// log line carries it, so the next slow file is diagnosed from the log
        /// alone. Never serialized — it describes the run, not the product.
        struct StageMs { double levels = 0.0, cards = 0.0, dag = 0.0; };
        QVector<StageMs> stageMs;   ///< aiScene mesh order, as `meshes`
        double meshesMs = 0.0;      ///< the wall time of all meshes' stages together
        int bakeThreads = 0;        ///< the width this bake ran at (MeshBake::bakeThreads when it started)
        /// "m0 99875t levels 12 cards 12 dag 567 | m1 ..." — the per-stage ms.
        QString stageSummary() const;
    };

    /// THE BAKE'S WIDTH (IMPORT-SPEED-1): how many threads one bake may use — the
    /// meshes of a model concurrently, and inside each mesh the bound's queries,
    /// the cluster DAG's group measurement and the SDF's exact band. 0 (the
    /// default) = 4, measured (BAKE-WIDTH-2: wider bakes hitch the editor's frames),
    /// capped at the hardware's thread count; the pool behind it is ONE process-
    /// wide set of workers at nice 19, the ONE core budget's background width
    /// (bakePoolThreads; THREADING-1), however many bakes run — a bake's caller
    /// works its own job too, so a width beyond the pool is slots, never threads.
    /// The OUTPUT does
    /// not depend on it — every reduction is ordered (meshbake.cpp, `bakepool`) —
    /// which is what `bake.determinism` proves byte for byte (1 thread against the
    /// hardware's).
    static void setBakeThreads(int threads);
    /// TEST ONLY (bake.determinism): the `chunks`-th unit of bake work from now on
    /// throws; buildFromScene must answer an invalid model and the next bake must
    /// be whole. 0 disarms.
    static void failBakeAfterChunksForTest(int chunks);
    static int bakeThreads();   ///< the width a bake started now would use
    /// The bake pool's worker threads (created once, with the pool): ThreadBudget::background.
    static int bakePoolThreads();

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

    /// ATOM-2 D10 — ONE SIMPLIFIER: build `mesh`'s WHOLE-MESH LEVEL TABLE (`lodIndices`, `lodBounds`, `lodErrors` —
    /// the stock casters, the voxeliser's level, the surface cache, the ray BLAS and the cards read it, unchanged) from
    /// its cluster DAG: at most kLevelTableMax UNIFORM-DEPTH CUTS — the level at depth d is every cluster born at or
    /// before d whose own group forms at or after d (the builder's pool at that level: a crack-free surface, its
    /// borders the locked ones), its triangles as one index list over the SAME vertex buffer, its bound the largest
    /// stored error among those clusters (a length in mesh units, non-decreasing with depth), its error the largest
    /// estimate of the groups that made them (a diagnostic). Depths 1 .. the root's, evenly spaced when there are more
    /// than kLevelTableMax; cleared when the mesh has no DAG. MUST RUN AFTER buildClusterDag on the same mesh.
    /// Pure CPU, no assimp, no engine; safe from any thread.
    static constexpr int kLevelTableMax = 8;
    static void buildLevelTable(const MeshPtr &mesh);

    /// SURFACE-CACHE phase 1 (SPECS/SURFACE_CACHE_ASSESSMENT.md §7): build
    /// `mesh`'s SURFACE CARD LIST, in place — `cards` + `cardCoverage`, or both
    /// cleared when the mesh gets none (it is skinned, it is not triangles, it
    /// has no area, or `maxCards` is zero).
    ///
    /// MUST RUN AFTER buildLevelTable on the same mesh: a card records the LOD
    /// level its own texel picks, and a mesh with no table yet would have every
    /// card name level 0.
    ///
    /// `maxCards` is the budget (ImportTransform::maxCards, default 12 — Epic's
    /// "Max Lumen Mesh Cards"); it is clamped to the format's ceiling of 64.
    /// Public for the same reason buildLevelTable is: the card list is a PRODUCT
    /// of the bake with its own policy, and a caller that builds an iris::Mesh by
    /// other means — a procedural mesh, a re-bake path, a suite — must be able to
    /// ask for the cards the importer would have produced rather than grow a
    /// second implementation of them.
    /// Pure CPU, no assimp, no engine; safe from any thread.
    static void buildCards(const MeshPtr &mesh, int maxCards);

    /// ATOM-2 (SPECS/ATOM_2_SPEC.md part A): build `mesh`'s CLUSTER DAG, in place — `clusterDag`, or an empty one
    /// when the mesh gets none (skinned, not triangles, malformed). The builder is import/atomdag/ (build.h states
    /// D1-D9); every triangle mesh gets one, ending in an empty top. The level table and the cards follow from it.
    /// Public for the reason the other products are: a caller that builds an iris::Mesh by other means — a
    /// procedural fixture, a suite — must get the DAG the importer would have produced, not a second implementation.
    /// Pure CPU, no assimp, no engine; safe from any thread. `options` exists for the tools and the rows (the POSONLY
    /// arm, the per-group records, the lock sets, the provenance); the bake passes the default.
    using ClusterDagStats = atomdag::Stats;
    using ClusterDagOptions = atomdag::Options;
    static void buildClusterDag(const MeshPtr &mesh, ClusterDagStats *stats = nullptr,
                                const ClusterDagOptions &options = ClusterDagOptions());
    /// THE DRIFT (a diagnostic, never stored — `test_mesh_bake --drift`): per depth d, the uniform cut at d (every
    /// cluster born at or before d whose own group forms at or after d) measured against level 0 both ways (the
    /// removed vertices and the reference pattern on every lost and every added facet, island-capped on level 0's
    /// side), beside the largest stored error the cut carries.
    struct DagDriftRow { int depth = 0; int triangles = 0; float drift = 0.0f; float stored = 0.0f; };
    static QVector<DagDriftRow> dagDrift(const MeshPtr &mesh);
    /// THE DRIFT PER CLUSTER (a diagnostic, never stored — `atom2.one_step_drift_bound`; PHOTON-7 stage 6: the ray
    /// origin's guard). Atom's cut draws a cluster when its PRODUCING group's stored error is under what the view affords,
    /// and every ray job lifts its origin off a drawn point by kLevelDriftOverBound x that error (jahshaka/engine/Types.h)
    /// - so the law holds per CLUSTER, not per uniform cut: per cluster a simplification produced (refined >= 0), the
    /// largest distance of its facets from level 0's surface (the ten fixed points of each facet level 0 does not have
    /// - the direction the lift clears: a drawn point inside level 0 by that much), beside its producing group's stored
    /// error.
    struct ClusterDriftRow { int cluster = 0; int depth = 0; float drift = 0.0f; float stored = 0.0f; };
    static QVector<ClusterDriftRow> clusterDrift(const MeshPtr &mesh);

    /// The capture resolution a baked card's LOD level was chosen for (Lumen's
    /// 128-texel page). Phase 2's atlas owns the page size it actually
    /// allocates; this is the number the BAKE assumed, so the two can be
    /// compared instead of silently disagreeing.
    static int cardCaptureResolution();

    // ---- THE CLIP BAKE (SHIPPED-BAKES-1) -----------------------------------
    //
    // An ANIMATION CLIP FILE (Studio's ModelTypes::Animation — a Mixamo
    // download "without skin", a .bvh capture) has no geometry for a mesh bake
    // and used to be PARSED by every reader: the avatar module's Load
    // Animation, avatar.loadClip, the scene reader's clip branch, the
    // thumbnail's pose strip and the metadata backfill. This is its built form,
    // a second KIND beside the model bake: the same producer key, the same
    // store role, its own magic and its own name.
    //
    // ONE BAKE PER CONTENT, BUILT AT IDENTITY. A clip's position keys have to
    // land on a rig baked under the CHARACTER's import settings, and one clip
    // drives many rigs — so the bake keeps the keys in the file's own units
    // (the file's declared unit applied, ImportFlags::ClipNamesOnly, exactly
    // what a clip read with an identity transform produced) and the rig's
    // uniform factor is applied when it is READ (`clipAnimations`), the one
    // thing assimp's ScaleProcess would have done to a position key with it.
    struct Clip
    {
        bool valid = false;
        QString fingerprint;
        /// Names, numbers, the node hierarchy and the pose-strip poses
        /// (ClipFileInfo::stripFractions()) of the parse.
        ClipFileInfo info;
        /// Mesh::extractAnimations of the parse, at identity.
        QMap<QString, SkeletalAnimationPtr> animations;
        /// Metres per source unit as the FILE declares it — what a rig's UNIT
        /// OVERRIDE is resolved against (import/scenesource.h) without a probe.
        double declaredUnitScale = 1.0;
    };

    /// The clip bake's key for a source whose content id is `sourceOid`.
    static QString clipFingerprintFor(const QString &sourceOid);
    /// `<oid16>-clip.jcb` — content only: the rig's settings are applied at read.
    static QString clipFileNameFor(const QString &sourceOid);
    /// Parse `filePath` (ImportFlags::ClipNamesOnly, identity) and build its
    /// clip bake. AN IMPORT-TIME / REBUILD CALL (source.assimp_import_only).
    static Clip buildClipFromFile(const QString &filePath, const QString &fingerprint);
    /// IrisGL-internal form (a complete aiScene needs assimp's headers).
    static Clip buildClipFromScene(const aiScene *scene, const QString &filePath,
                                   const QString &fingerprint);
    static QByteArray serializeClip(const Clip &clip);
    static Clip deserializeClip(const QByteArray &blob,
                                const QString &expectFingerprint = QString());
    static Clip readClip(const QString &path, const QString &expectFingerprint = QString());
    static bool writeClip(const QString &path, const Clip &clip, QString *errorOut);
    static bool clipHeaderMatches(const QString &path, const QString &expectFingerprint);
    /// The clip's animations for a RIG imported under `rig` (its keysOnly()
    /// transform): FRESH copies every call (a caller owns what it is handed),
    /// `source` stamped on each, position keys scaled by the rig's uniform
    /// factor exactly as readSceneFile would have handed it to assimp.
    static QMap<QString, SkeletalAnimationPtr> clipAnimations(const Clip &clip,
                                                              const ImportTransform &rig,
                                                              const QString &source);

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
    ///
    /// `cancel` (STAGE-CLOSE-TRIAGE-1 F5): when it reads true the bake stops at
    /// its next cancel point — between meshes, before the stages, before each
    /// stage task on the pool — and answers an invalid model (logged
    /// "cancelled"). A cancel point changes no output: a bake that is not
    /// cancelled is the same bytes.
    static Model buildFromScene(const aiScene *scene, const QString &filePath,
                                const QString &fingerprint,
                                const QString &extractDir = QString(),
                                const ImportTransform &xf = ImportTransform(),
                                const std::atomic<bool> *cancel = nullptr);

    /// Parse `filePath` and bake it. Used by the lazy re-bake of an existing
    /// library, where no parse is in flight. `cancel` also reaches assimp's
    /// parse (a ProgressHandler whose Update() answers false aborts ReadFile).
    static Model buildFromFile(const QString &filePath, const QString &fingerprint,
                               const QString &extractDir = QString(),
                               const ImportTransform &xf = ImportTransform(),
                               const std::atomic<bool> *cancel = nullptr);

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

    /// THE FACTS OF THE IMPORT'S PARSE alone (Model::describe), read from the
    /// bake's HEADER — the read stops there, no geometry is deserialized. What
    /// the metadata backfill describes a model from. False on a blob of another
    /// format, another key (when one is given) or a truncated header.
    static bool readDescribe(const QString &path, ModelSceneInfo *out,
                             const QString &expectFingerprint = QString());

    /// Write ATOMICALLY (temp + rename in the same directory): a bake at its
    /// final path is either absent or complete, even through a SIGKILL.
    static bool write(const QString &path, const Model &model, QString *errorOut);

    /// The baked equivalent of `MeshNode::loadAsSceneFragment` — same node
    /// shape, same materials, same animations, no assimp.
    static SceneNodePtr buildFragment(
        const Model &model, const QString &filePath,
        const std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData &data)> &createMaterialFunc);

private:
    /// buildFromScene's body; buildFromScene turns any exception out of it into
    /// an invalid model (a bake that throws is no bake).
    static Model buildFromSceneUnguarded(const aiScene *scene, const QString &filePath,
                                         const QString &fingerprint, const QString &extractDir,
                                         const ImportTransform &xf, const std::atomic<bool> *cancel);
};

/// A read clip bake, shared (Studio's MeshBakeStore::loadClip).
using BakedClipPtr = std::shared_ptr<const MeshBake::Clip>;

}   // namespace iris

#endif   // MESHBAKE_H
