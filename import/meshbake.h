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
        /// WHAT THE BAKE COST, per mesh and per stage (IMPORT-SPEED-1): the import's
        /// log line carries it, so the next slow file is diagnosed from the log
        /// alone. Never serialized — it describes the run, not the product.
        struct StageMs { double lodChain = 0.0, cards = 0.0, sdf = 0.0, dag = 0.0; };
        QVector<StageMs> stageMs;   ///< aiScene mesh order, as `meshes`
        double meshesMs = 0.0;      ///< the wall time of all meshes' stages together
        /// "m0 99875t lod 1234 cards 12 sdf 34 dag 567 | m1 ..." — the per-stage ms.
        QString stageSummary() const;
    };

    /// THE BAKE'S WIDTH (IMPORT-SPEED-1): how many threads one bake may use — the
    /// meshes of a model concurrently, and inside each mesh the bound's queries,
    /// the cluster DAG's group measurement and the SDF's exact band. 0 (the
    /// default) = the hardware's thread count; the pool behind it is ONE process-
    /// wide set of hardware-size workers, however many bakes run. The OUTPUT does
    /// not depend on it — every reduction is ordered (meshbake.cpp, `bakepool`) —
    /// which is what `bake.determinism` proves byte for byte (1 thread against the
    /// hardware's).
    static void setBakeThreads(int threads);
    static int bakeThreads();   ///< the width a bake started now would use

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
    ///
    /// `terms`, when given, receives ONE ROW PER STORED LEVEL: how its bound was
    /// made (the bound's two measured terms and the island rule's work). It is the
    /// test-time window on the rule — the suites print it; the bake passes null.
    struct LodLevelTerms
    {
        int   triangles = 0;
        float quadric = 0.0f;        ///< the simplifier's own error (lodErrors)
        float areaTerm = 0.0f;       ///< sampled two-sided distance, margin applied
        float vertexTerm = 0.0f;     ///< exact removed-vertex distance (island-capped)
        float facetTerm = 0.0f;      ///< exact per-facet distance: dropped/added facets' centroid + edge midpoints (at least vertexTerm)
        float bound = 0.0f;          ///< what lodBounds stores
        int   islandsDropped = 0;    ///< components level 0 has and this level has none of
        float droppedMaxExtent = 0.0f;  ///< the largest of those (its bbox diagonal)
        int   verticesLocked = 0;    ///< removed vertices the displacement lock kept (displaced past the budget)
        int   passes = 0;            ///< simplifier runs this level took (1 = no island had to be kept)
        int   worstComponentTriangles = 0;   ///< level-0 triangles of the component the vertex term came from
        float worstComponentExtent = 0.0f;   ///< ...and its extent
    };
    static void buildLodChain(const MeshPtr &mesh, QVector<LodLevelTerms> *terms = nullptr);

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
    /// of the bake with its own policy, and a caller that builds an iris::Mesh by
    /// other means — a procedural mesh, a re-bake path, a suite — must be able to
    /// ask for the cards the importer would have produced rather than grow a
    /// second implementation of them.
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

    /// ATOM stage 2 (SPECS/atom/B2_CLUSTER_DAG_DESIGN.md §1): build `mesh`'s
    /// CLUSTER DAG, in place — `clusterDag`, or an empty one when the mesh gets
    /// none (skinned, not triangles, or under two leaf clusters of triangles).
    ///
    /// Independent of the chain (it simplifies level 0 itself) and of the cards
    /// and the field; buildFromScene runs it last. Public for the reason the
    /// other three products are: a caller that builds an iris::Mesh by other
    /// means — a procedural fixture, a suite — must get the DAG the importer
    /// would have produced, not a second implementation of it.
    /// Pure CPU, no assimp, no engine; safe from any thread.
    ///
    /// `variant` exists for the ONE measurement that chose the shipped config
    /// (tests/atom/cluster_config_measure.cpp); every other caller takes the
    /// default, which is the only config a bake is ever written with — the blob
    /// records it, and a blob written under another is refused and re-baked.
    enum class ClusterDagVariant {
        Shipped,               ///< what the bake writes (see meshbake.cpp, clusterdag::config)
        DefaultProtectUv,      ///< clodDefaultConfig(128), permissive, UV seams PROTECTED (the header's pairing)
        DefaultProtectAll,     ///< ...normal AND UV discontinuities protected
        PermissiveCharged,     ///< permissive, NOTHING protected: seams paid for in the error (stage 1's choice)
        RegularizedProtectUv,  ///< DefaultProtectUv + simplify_regularize
        Strict,                ///< non-permissive, permissive only as a fallback (seams locked by topology)
        Count
    };
    /// What one build did, for the report and the suites. `clusterRegions`
    /// (filled only when asked) is the PROVENANCE the measurement computed: for
    /// every cluster, the level-0 triangles (indices into the mesh's index list,
    /// divided by 3) it stands for — the regions of one group's outputs partition
    /// the union of its members' regions, which is what makes "exactly one cut
    /// covers every level-0 triangle" checkable (atom.cluster_cut).
    struct ClusterDagStats
    {
        int    clusters = 0;
        int    groups = 0;
        int    depth = 0;              ///< max group depth + 1
        int    terminalGroups = 0;
        int    monotoneFixes = 0;      ///< groups whose measured error was raised to a child's
        int    sphereFixes = 0;        ///< groups whose sphere was grown to contain a child's
        int    measuredBelowEstimate = 0;  ///< groups where the measurement came in under clusterlod's number
        int    localFallbacks = 0;     ///< term-1 samples the group's local soup could not answer (exact either way)
        /// IN: the REFERENCE measurement — term 1 against the WHOLE level-0 grid and
        /// every area sample's plain nearest distance (no early out) — which the
        /// shipped path (a local soup + max-only queries) is exact against;
        /// atom.cluster_cut asserts the two agree.
        bool   referenceMeasure = false;
        double buildMs = 0.0;          ///< clodBuild alone
        double measureMs = 0.0;        ///< the per-group measurement + provenance
        bool   wantRegions = false;
        QVector<QVector<quint32>> clusterRegions;
    };
    static void buildClusterDag(const MeshPtr &mesh, ClusterDagStats *stats = nullptr,
                                ClusterDagVariant variant = ClusterDagVariant::Shipped);
    /// The variant the bake writes (the measurement's pick).
    static ClusterDagVariant shippedClusterDagVariant();
    static const char *clusterDagVariantName(ClusterDagVariant variant);

    /// VERIFICATION, and it exists for the same reason `producerHashOf` does: the
    /// claim has to be TESTABLE without re-running the thing that made it.
    ///
    /// `checkLodBounds` re-measures every level of `mesh` against level 0 — PER
    /// FACET (every dropped level-0 facet and every level facet at its corners,
    /// edge midpoints, centroid and the centroids of its 1-to-4 split: strictly
    /// denser than the bake's centroid + midpoints), plus `densityMultiple` times
    /// the bake's area samples and the exact removed-vertex walk, under the same
    /// represented-surface rule (island caps) and WITHOUT the sampling margin —
    /// and answers whether every stored `lodBounds[k]` is at least that. On a mesh with no islands (one
    /// component: every shipped primitive, the dragon) the reference IS the
    /// sampled two-sided Hausdorff distance.
    ///
    /// WHAT IT IS AND IS NOT. The exact vertex term is the same computation the
    /// bake made, so where it carries the maximum the check reproduces the stored
    /// number and passes at ratio 1.0000; what it genuinely tests is that the area
    /// margin covers what 8x the samples find inside facets, and the whole
    /// pipeline (sampler, grid, floor, monotonicity). A mesh with no chain
    /// trivially passes. `worstRatioOut` receives the largest (reference / stored)
    /// seen, and `referenceOut` one reference per level.
    static bool checkLodBounds(const MeshPtr &mesh, int densityMultiple = 8,
                               double *worstRatioOut = nullptr,
                               QVector<float> *referenceOut = nullptr);

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
