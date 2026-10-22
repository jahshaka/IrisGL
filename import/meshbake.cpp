/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "import/meshbake.h"
#include "import/parsecensus.h"
#include "core/threadbudget.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <stdexcept>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>
#include <pthread.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#if defined(__GLIBC__)
#include <malloc.h>   // malloc_trim — bakeStages hands the pool threads' freed arenas back
#endif

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "assimp/Importer.hpp"
#include "assimp/ProgressHandler.hpp"
#include "assimp/scene.h"

// ATOM stage 1 (SPECS/NANITE_SPEC.md §7): the LOD chain is built HERE, once, at
// import. Vendored at thirdparty/meshoptimizer, pinned to the release tag v1.2;
// this and the cluster DAG's builder (import/atomdag/, ATOM-2) are the only
// translation units that include it.
#include "meshoptimizer.h"
#include "import/atomdag/build.h"
#include "import/bakeshared.h"
#include <chrono>
#include <unordered_map>

#include "core/geometry/trimesh.h"
#include "core/logger.h"
#include "core/math/trs.h"
#include "document/animation/animation.h"
#include "document/animation/skeletalanimation.h"
#include "document/assets/skeleton.h"
#include "document/assets/vertexbuffer.h"
#include "document/scenegraph/meshnode.h"
#include "document/scenegraph/scenenode.h"
#include "import/importflags.h"
#include "import/importsettings.h"
#include "import/materialhelper.h"
#include "import/modelsceneinfo.h"

namespace iris
{

namespace
{

// The producer hash is a compile-time constant when CMake supplies it (the
// engine's shader-cache mechanism, applied to the TUs that BUILD a bake). A
// build without it still works: the term becomes "dev", so a developer's local
// bakes stay valid across edits — which is exactly the "recompile and the cache
// dies" cost the define exists to avoid paying in CI/release.
#ifndef JAHSHAKA_MESH_BAKE_PRODUCER_ID
#define JAHSHAKA_MESH_BAKE_PRODUCER_ID "dev"
#endif

// The same list, as text, so the suite can check that what CMake hashes is what
// this build reports — and that the document-side classes are NOT in it.
// '|' separated (a ';' inside a compile definition is a CMake list separator).
#ifndef JAHSHAKA_MESH_BAKE_HASHED_SOURCES
#define JAHSHAKA_MESH_BAKE_HASHED_SOURCES ""
#endif

// v2 (2026-09-08): materials carry `unlit`, and the shading factors changed
// MEANING — a v1 blob recorded the full-metal misreading of every spec-gloss
// and block-less glTF material, so v1 bakes must be re-baked, not replayed.
// v3 (2026-09-09): `nodeName` left MeshMaterialData. It was never written by an
// importer and never read by anything — this serializer was its ONLY mention in
// the tree — but it occupied a length-prefixed string in every material record,
// so removing it is a layout change. (The producer id already covers this file,
// so an existing bake would have been rebuilt anyway; the bump makes the reason
// legible in the header instead of only in a hash.) Shading factors and their
// meaning are UNCHANGED from v2.
// v4 (2026-09-11, smoke L10 item 2): every clip record carries the file's
// DECLARED length (a double, after the clip name) — the only record of a
// one-frame clip's length once the preset has merged its identical keys. A v3
// blob replayed would bring the Mixamo T-pose clip back at zero seconds.
// v5 (2026-09-13, SKY_LIGHT_SPEC.md §4 / round-2 review item 2): the MEANING of
// every stored QColor changed. An imported material's colours are LINEAR at
// their source (glTF says so for baseColorFactor, emissiveFactor and
// specularColorFactor; assimp hands them through) and the importer used to put
// the raw float into a QColor, which the renderer now decodes as sRGB — so a
// bake made before the encode landed replays a colour that darkens by a gamma.
// The LAYOUT is unchanged; the bump exists to re-bake every asset that carries
// the old meaning, which no fingerprint could have caught (the source file did
// not change, the reader of it did).
// v6 (2026-09-15, BAKEKEY-1): the KEY ITSELF was redefined. The producer hash
// used to cover twelve files, including the document's mesh/mesh-node/skeleton
// classes, which every lane edits for reasons that cannot move a baked byte —
// measured: 25 of the 408 irisgl commits since 2026-09-05 touched one, on 10 of
// 11 working days, so nearly every build staled every bake in every library.
// The hash is now the three files that WRITE a bake, and THIS CONSTANT carries
// the document-side layout and meaning instead (irisgl/CMakeLists.txt has the
// full reasoning; tests/hygiene/bake_key_guard.sh is the gate that keeps the
// hand bump from being forgotten). The LAYOUT is unchanged from v5 — the bump
// exists because the key's DEFINITION changed, so every bake in every library
// is rejected and rebuilt exactly once more, under the new key.
// v7 (2026-09-15, ATOM stage 1, SPECS/NANITE_SPEC.md §7): every mesh record
// carries a TRAILING LOD BLOCK — the automatic simplification chain built at
// import (levelCount, then per level its index list and its simplifier error —
// position + attribute quadrics combined, >= the geometric error — as
// a length in mesh units). A v6 blob has no chain at all, so replaying one
// would silently ship a library whose models never drop a triangle, with no way
// for any fingerprint to notice; and the block is a layout change besides.
// v8 (2026-09-16, IMPORT-1, SPECS/IMPORT_DIALOG_SPEC.md §4.4): the MEANING of
// baked geometry changed. An asset's scale, rotation and origin are decided
// ONCE at import and BAKED (the import-settings record, import/importsettings.h),
// so a bake is no longer "the file as authored" but "the file as this asset's
// import settings transformed it" — and the single-mesh shortcut no longer
// carries the file's own node transform on the fragment root, it folds it into
// the vertices and emits an identity root. Neither is visible to any
// fingerprint: the source bytes did not change, the reader of them did. The key
// gains a SETTINGS term at the same time (fingerprintFor/fileNameFor below), so
// two imports of one source with different settings stop colliding; the bump is
// what rejects every bake produced under the old meaning.
//
// v9 (2026-09-17, ATOM-3, the render audit's A6): the LOD CHAIN GOES ALL THE WAY
// DOWN. `kMaxLevels = 4` ended every chain at 1/16 of the authored triangles, so
// no asset had a far-field proxy; the chain now halves until the triangle floor
// (`kMinTriangles`, 128 — Nanite's root cluster), the accept ratio or the 5 %
// error cap stops it. Every mesh above 256 triangles gets MORE levels than its
// old bake, with the same errors for the levels it already had, so the bytes of
// the trailing LOD block change for most library assets. (`meshbake.cpp` is in
// the producer hash and would re-bake every library on its own; the bump is what
// makes the reason readable, and it is the version an old .jmb is rejected by.)
// v10 (2026-09-21, SURFACE-CACHE-1a, the surface cache's phase 1): every mesh
// now carries a CARD LIST — the axis-aligned orthographic captures phase 2 will
// run and phase 4 will read (document/assets/mesh.h MeshCard, the generator
// beside the LOD chain below). It is a new trailing block on every mesh, so
// every .jmb written before today is a shorter blob than this build reads and
// is rejected by this line; and the import record gained `maxCards`, which is
// part of the bake key through ImportSettings (an import at a different card
// budget is a different bake, exactly as a different scale is). Every library
// re-bakes once, on purpose, by the BAKEKEY-1 rule — the bake's OUTPUT changed,
// so the version is bumped rather than the commit carrying `bake-output:
// unchanged`.
// v12 (2026-09-22, ATOM-BAKE-1, ATOM P1's AT-A5 + P2's AT-A4): THE LEVEL ERROR
// IS MEASURED, NOT CLAIMED, and EVERY ASSET IS BAKED. Each level of the chain
// now carries a second length beside the simplifier's — `lodBounds`, a sampled
// two-sided distance between that level and level 0 — and it is the one every
// consumer reads (the view's pixel rule, the cascade's cell rule, the card's
// texel rule). The simplifier's own number stays as a diagnostic. Every mesh
// also carries a SIGNED DISTANCE FIELD (a mesh-relative int8 grid, jump-flooded
// over the level-0 soup) as a new trailing block; nothing reads it yet. Two new
// trailing blocks and a card level that can now name a different level: every
// .jmb written before today is rejected by this line, on purpose, and every
// library re-bakes once (the BAKEKEY-1 rule — the bake's OUTPUT changed, so the
// version is bumped rather than the commit carrying `bake-output: unchanged`).
// v13 (2026-09-23, ATOM-CLUSTER-1, ATOM stage 2): EVERY MESH CARRIES A CLUSTER
// DAG beside its chain — clodBuild's clusters and groups over level 0, stored
// meshlet-local, each group with its MEASURED error beside clusterlod's own
// estimate, and the config the DAG was built under (a blob built under another
// config is refused). A new trailing block on every mesh: every .jmb written
// before today is rejected by this line, on purpose, and every library re-bakes
// once (the BAKEKEY-1 rule — the output changed, so the version is bumped).
// v14 (2026-09-26, ATOM-LOD-BOUND-1): THE BOUND MEASURES REPRESENTED SURFACE, and
// the chain honours it. A dropped island costs its own extent (never its distance
// to unrelated surface), the sampling margin multiplies the sampled term only, and
// a level is re-simplified with every vertex it would displace past twice its own
// error locked (lodchain::build). On smooth content (the primitives, the dragon)
// the levels are the same index lists with bounds up to 1/1.25 smaller; on a scan
// both the levels and the bounds change (the owner's temple: level-1 bound 2.48 m
// -> 0.047 m). The bake's OUTPUT changed, so the version is bumped (BAKEKEY-1).
// v15 (2026-09-26, ATOM-CLUSTER-CUT): THE CLUSTER DAG'S GROUP ERRORS FOLLOW THE
// BOUND'S RULE. Every group's level-0 side is capped at its island's extent (a
// dropped debris island costs its size, never its distance to unrelated surface —
// the owner's temple's groups read 8-60x their simplifier error at the median, up
// to 520x), the sampling margin multiplies the sampled terms only, and an exact
// per-facet walk (seven points a facet) joins the removed-vertex walk; the groups
// sample 8 points per simplified triangle and 4 per lost facet. Every stored group
// error can move, so the version is bumped (BAKEKEY-1).
// v16 (2026-09-28, CULL-MODE-1): a baked material carries its source's TWO-SIDED
// flag (glTF doubleSided -> AI_MATKEY_TWOSIDED -> MeshMaterialData::twoSided), and
// the fragment writes it onto the MeshNode as faceCullingMode None.
// v17 (2026-09-28, DAG-LOCK-1): THE CLUSTER DAG HAS THE CHAIN'S DISPLACEMENT LOCK. A
// removed vertex or a lost facet past 2x its group's own simplifier error, or an island
// bigger than the group's error, is locked with its fan and the whole DAG rebuilt (at
// most 16 builds; clusterlod's lock is one array per build). The primitives with no
// lock taken keep their bytes; hp_sphere, hemisphere and every scan re-bake.
// v18 (2026-09-29, CLUSTER-LOCK-1): THE DAG'S LOCK IS BY POSITION. A locked vertex
// locks every index at its position (a seam's, a pole's twins) and the fan of each; a
// twin left free was moved by clusterlod's sloppy fallback and opened the cut. Every
// mesh whose lock touched a shared position re-bakes (hp_sphere, hemisphere, scans).
// v19 (2026-09-29, CLUSTER-LOCK-3): THE DAG'S LOCK IS PER GROUP. clusterlod verifies
// each group as it simplifies it (patch 0003) and re-simplifies THAT group with its
// offenders locked (at most 4 times; a group stuck under its locks is output terminal);
// nothing carries to another group or level, and the whole-DAG rebuild is gone. Patch
// 0002 keeps a terminal group's border locked. The lost facets are walked exactly
// (`facetMax`). Roots collapse again (uv-sphere-20k 2916 -> 76 triangles, physics-model
// 306 -> 76, round-bar-40m 4806 -> 24); every DAG re-bakes.
// v20 (2026-10-01, SHIPPED-BAKES-1): THE BAKE CARRIES THE FACTS OF ITS PARSE.
// Every model bake gains a trailing DESCRIBE block (ModelSceneInfo: counts,
// texture references, bone and node names, the clip table, the extent, the
// declared unit) so the metadata backfill reads a bake instead of parsing; and a
// second bake KIND, the CLIP bake ('JCLP', MeshBake::Clip), is written under the
// same version for every animation clip file. The container changed, so every
// .jmb written before today is rejected by this line and every library re-bakes
// once (BAKEKEY-1).
// v21 (2026-10-13, BISTRO-IMPORT-1): A BAKED MATERIAL CARRIES ITS COVERAGE — alphaMode,
// alpha and alphaCutoff (an FBX opacity below 1 is glass; a base-colour map with alpha
// holes is a cutout, and two-sided), the FBX diffuse is the base colour when the file
// states no BASE_COLOR, and a prepared `_Specular_{roughness,metallic}.png` pair binds
// when the named `_Specular.png` is absent. Material bytes change (BAKEKEY-1).
// v22 (2026-10-14, BISTRO-FIXES-2): A BOUND MAP IS THE VALUE — a non-glTF material
// with a roughness (metallic) map that is really there bakes roughnessFactor
// (metallicFactor) 1, never the file's scalar beside it (the renderer multiplies the
// two: the Bistro's 0.5528 x 0.737 = 0.41); and separate METALNESS / DIFFUSE_ROUGHNESS
// maps (OBJ map_Pm / map_Pr, Stingray/Max FBX) bind as themselves — only glTF's one
// packed image is split. Material bytes change (BAKEKEY-1).
// v23 (2026-10-21, ATOM-6): THE DAG REACHES ITS ROOT — clusterlod patch 0004's lock-aware prune: a group still
// above its halving target after the collapses removes its UNLOCKED components whole, smallest island first, so a
// group of leaf cards or closed boxes keeps simplifying instead of ending terminal or sloppy; a group pruned to
// nothing is a GROUP with its measured error (the largest removed island's extent) and no clusters, never a
// terminal. Every mesh with an unlocked component in a stuck group re-bakes: pruned levels, empty groups, the
// blob's config record (simplify_prune_unlocked). The output changed, so the version is bumped (BAKEKEY-1).
// v24 (2026-10-21, ATOM-7 stage 1): THE LOCK READS THE CHILDREN — the per-group displacement lock's bound is
// kDagLockFactor x the error the group's CHILDREN carry as the verify measured them (the quadric estimate only at
// depth 0, where it is honest), and the prune's clause (2) — on toward the halving past 0.85 — is decided by the
// verify under an absolute budget (clusterlod patch 0005); clodConfig::simplify_prune_budget leaves the config
// record. Every DAG re-bakes (BAKEKEY-1).
// v25 (2026-10-21, ATOM-7 stage 2): A LOCKED POSITION SURVIVES — the verify's rule 4: a position the level locked
// that a group's result no longer uses locks its fan and the group is retried (an attempt that keeps its border wins
// the best-attempt tie); CLOD_LOCK_VERIFY public in patch 0005. Every DAG re-bakes (BAKEKEY-1).
// v26 (2026-10-22, ATOM-2 stage 1): THE CLUSTER DAG IS OUR OWN BUILDER (import/atomdag/; clusterlod.h, its patch
// stack 0002-0005 and the old verify/measure deleted): adjacency by position, 128-triangle / 256-vertex leaves and
// 8-32 groups by our own bisection, borders recomputed every level, Nanite-scale attribute weights, the measured
// one-step price (output vs the group's input, exact both ways) and the ladder (2x the children; a group that cannot
// is carried, never forced), every triangle mesh with a DAG ending in an empty top. The per-mesh SDF is gone (nothing
// read it). The blob's DAG config record is the builder's. Every bake re-bakes (BAKEKEY-1).
// v27 (2026-10-22, ATOM-2 stage 2 + the stage-1 read): the empty top priced at the mesh's level-0 extent; a group's
// result accepted when it fits its budget and needs FEWER CLUSTERS (the 15 % shed rule deleted); the islands over the
// adjacency's weld ids (+-0 folded); the partitioner's part count vertex-aware (stage 2's placement — survivors
// appended at their quadric optimum — measured a no-go by the memory rule and deleted). Every bake re-bakes (BAKEKEY-1).
// v28 (2026-10-22, ATOM-2 the stage-2 read): A CLUSTER-COUNT CARRY KEEPS ITS BUDGET — a group with a result within
// its budget that cannot reduce its cluster count is carried at the SAME budget (only a budget failure, or a level
// where every group carried, doubles); carried leaf groups no longer take a depth-0 estimate. Every bake re-bakes
// (BAKEKEY-1).
// v29 (2026-10-22, ATOM-2 stage 3): D5 THE WELD (snapped positions of different islands within E / 2) and two weight
// arms, measured; both resolved in v30. Every bake re-bakes (BAKEKEY-1).
// v30 (2026-10-22, ATOM-2 the lead's word on stage 3): ONE BAKE — every attribute at a tiny weight (POSONLY, the
// picked arm: 27,015 against 42,228 on the hard 7 at E = 16); the Nanite-scale arm, its switch and D5's weld deleted
// (the weld laid shells onto each other as coincident sheets the simplifier cannot reduce). Every bake re-bakes.
// v31 (2026-10-22, ATOM-2 stage 4): D9 FOLIAGE'S AGGREGATE — a group mostly of cards (islands under 12 triangles)
// merges neighbouring cards into quads of their summed area, priced by the measured deviation; each quad is four
// vertices appended to the mesh (the only vertices the DAG adds). Every bake re-bakes (BAKEKEY-1).
constexpr int kFormatVersion = 31;
constexpr quint32 kMagic = 0x4A4D424Bu;       // 'JMBK'
constexpr quint32 kClipMagic = 0x4A434C50u;   // 'JCLP'

/// QDataStream settings are PINNED: the same Model must serialize to the same
/// bytes on every machine and every Qt version, because the bake's content id
/// is what assets.checkConsistency compares.
void configure(QDataStream &s)
{
    s.setVersion(QDataStream::Qt_6_0);
    s.setByteOrder(QDataStream::LittleEndian);
    s.setFloatingPointPrecision(QDataStream::SinglePrecision);
}

void writeColor(QDataStream &s, const QColor &c)
{
    s << qint32(c.isValid() ? 1 : 0) << qint32(c.red()) << qint32(c.green())
      << qint32(c.blue()) << qint32(c.alpha());
}

QColor readColor(QDataStream &s)
{
    qint32 valid = 0, r = 0, g = 0, b = 0, a = 255;
    s >> valid >> r >> g >> b >> a;
    if (!valid) return QColor();
    return QColor(r, g, b, a);
}

void writeMatrix(QDataStream &s, const iris::Mat4 &m)
{
    const float *d = m.constData();
    for (int i = 0; i < 16; ++i) s << float(d[i]);
}

iris::Mat4 readMatrix(QDataStream &s)
{
    float d[16];
    for (int i = 0; i < 16; ++i) s >> d[i];
    return iris::Mat4(d[0], d[4], d[8],  d[12],
                      d[1], d[5], d[9],  d[13],
                      d[2], d[6], d[10], d[14],
                      d[3], d[7], d[11], d[15]);
}

void writeVec3(QDataStream &s, const iris::Vec3 &v) { s << float(v.x()) << float(v.y()) << float(v.z()); }
iris::Vec3 readVec3(QDataStream &s) { float x, y, z; s >> x >> y >> z; return iris::Vec3(x, y, z); }

void writeQuat(QDataStream &s, const iris::Quat &q)
{
    s << float(q.scalar()) << float(q.x()) << float(q.y()) << float(q.z());
}
iris::Quat readQuat(QDataStream &s)
{
    float w, x, y, z; s >> w >> x >> y >> z; return iris::Quat(w, x, y, z);
}

// ---- materials -------------------------------------------------------------

void writeMaterial(QDataStream &s, const MeshMaterialData &m)
{
    writeColor(s, m.diffuseColor);
    writeColor(s, m.specularColor);
    writeColor(s, m.ambientColor);
    writeColor(s, m.emissionColor);
    s << float(m.shininess);
    s << m.diffuseTexture << m.specularTexture << m.normalTexture << m.hightTexture;
    s << qint32(m.hasEmbeddedDiffTexture) << qint32(m.hasEmbeddedSpecularTexture)
      << qint32(m.hasEmbeddedNormalTexture) << qint32(m.hasEmbeddedHightTexture);
    s << qint32(m.hasPbr) << qint32(m.unlit);
    writeColor(s, m.baseColorFactor);
    s << float(m.metallicFactor) << float(m.roughnessFactor);
    s << m.baseColorTexture << m.metallicTexture << m.roughnessTexture << m.emissiveTexture;
    s << qint32(m.twoSided);
    s << qint32(m.alphaMode) << float(m.alpha) << float(m.alphaCutoff);
}

MeshMaterialData readMaterial(QDataStream &s)
{
    MeshMaterialData m;
    m.diffuseColor = readColor(s);
    m.specularColor = readColor(s);
    m.ambientColor = readColor(s);
    m.emissionColor = readColor(s);
    s >> m.shininess;
    s >> m.diffuseTexture >> m.specularTexture >> m.normalTexture >> m.hightTexture;
    qint32 e0, e1, e2, e3, pbr, unlit;
    s >> e0 >> e1 >> e2 >> e3 >> pbr >> unlit;
    m.hasEmbeddedDiffTexture = e0;
    m.hasEmbeddedSpecularTexture = e1;
    m.hasEmbeddedNormalTexture = e2;
    m.hasEmbeddedHightTexture = e3;
    m.hasPbr = pbr;
    m.unlit = unlit;
    m.baseColorFactor = readColor(s);
    s >> m.metallicFactor >> m.roughnessFactor;
    s >> m.baseColorTexture >> m.metallicTexture >> m.roughnessTexture >> m.emissiveTexture;
    qint32 twoSided = 0;
    s >> twoSided;
    m.twoSided = twoSided != 0;
    qint32 alphaMode = 0;
    s >> alphaMode >> m.alpha >> m.alphaCutoff;
    m.alphaMode = alphaMode;
    return m;
}

// ---- skeletons -------------------------------------------------------------
//
// Only the NAME, the offset matrix and the parent link are stored. Everything
// else (meshSpacePoseMatrix, localMatrix, the bind TRS) is re-derived by the
// SAME arithmetic Mesh::extractSkeleton runs, so a baked rig is identical to a
// parsed one by construction rather than by promise — and the format cannot
// drift out of step with the derivation.

void writeSkeleton(QDataStream &s, const SkeletonPtr &skel)
{
    if (skel.isNull()) { s << qint32(-1); return; }
    s << qint32(skel->bones.size());
    QHash<Bone *, int> index;
    for (int i = 0; i < skel->bones.size(); ++i) index.insert(skel->bones[i].data(), i);
    for (const BonePtr &bone : skel->bones) {
        s << bone->name;
        writeMatrix(s, bone->inverseMeshSpacePoseMatrix);
        s << qint32(bone->parentBone.isNull() ? -1 : index.value(bone->parent().data(), -1));
    }
}

SkeletonPtr readSkeleton(QDataStream &s, bool *okOut)
{
    qint32 count = 0;
    s >> count;
    if (count < 0) return SkeletonPtr();
    if (count > 65536) { *okOut = false; return SkeletonPtr(); }
    auto skel = Skeleton::create();
    QVector<qint32> parents(count, -1);
    for (qint32 i = 0; i < count; ++i) {
        QString name;
        s >> name;
        auto bone = Bone::create(name);
        bone->inverseMeshSpacePoseMatrix = readMatrix(s);
        bone->meshSpacePoseMatrix = bone->inverseMeshSpacePoseMatrix.inverted();
        s >> parents[i];
        skel->addBone(bone);
    }
    if (s.status() != QDataStream::Ok) { *okOut = false; return SkeletonPtr(); }
    for (qint32 i = 0; i < count; ++i) {
        const qint32 p = parents[i];
        if (p < 0) continue;
        if (p >= count || p == i) { *okOut = false; return SkeletonPtr(); }
        skel->bones[p]->addChild(skel->bones[i]);
    }
    // The bind local of every bone — the identical loop Mesh::extractSkeleton
    // runs after linking (ANIMATION_ENGINE_MIGRATION_SPEC §1.5 F1).
    for (const auto &bone : skel->bones) {
        const iris::Mat4 bindLocal = !bone->parentBone.isNull()
            ? bone->parent()->inverseMeshSpacePoseMatrix * bone->meshSpacePoseMatrix
            : bone->meshSpacePoseMatrix;
        decomposeTRS(bindLocal, bone->bindingPos, bone->bindingRot, bone->bindingScale);
        bone->localMatrix = bindLocal;
        bone->pos = bone->bindingPos;
        bone->rot = bone->bindingRot;
        bone->scale = bone->bindingScale;
    }
    return skel;
}

// ---- the cluster DAG (format v13) -------------------------------------------
//
// The CONFIG RECORD is the builder's (atomdag::configRecord), and the reader
// refuses a blob written under any other.

void writeClusterDag(QDataStream &s, const MeshClusterDag &dag)
{
    s << qint32(dag.clusters.size());
    if (dag.isEmpty()) return;
    const QVector<float> record = atomdag::configRecord();
    s << qint32(record.size());
    for (float f : record) s << f;
    s << qint32(dag.groups.size());
    for (const MeshClusterDag::Group &g : dag.groups) {
        s << qint32(g.depth) << g.centre[0] << g.centre[1] << g.centre[2] << g.radius
          << g.error << g.estimate;
    }
    for (const MeshClusterDag::Cluster &c : dag.clusters) {
        s << quint32(c.vertexOffset) << quint32(c.triangleOffset) << quint16(c.vertexCount)
          << quint16(c.triangleCount) << qint32(c.group) << qint32(c.refined)
          << c.centre[0] << c.centre[1] << c.centre[2] << c.radius;
    }
    s << QByteArray(reinterpret_cast<const char *>(dag.vertices.constData()),
                    dag.vertices.size() * int(sizeof(quint32)));
    s << dag.triangles;
}

/// Every field is checked, for the reason the chain's levels are: a consumer
/// reads this as geometry and as a SELECTION RULE. A range outside its arrays,
/// a local index past its cluster's vertices, a vertex the mesh does not have, a
/// link to a group that does not exist, a non-finite sphere or error, or an
/// error that FALLS from a child group to its parent (the rule is only
/// crack-free over a monotone DAG) — each refuses the blob, and a blob written
/// under another config is refused too, so the asset re-bakes.
bool readClusterDag(QDataStream &s, int vertexCount, MeshClusterDag *out)
{
    *out = MeshClusterDag();
    qint32 clusterCount = 0;
    s >> clusterCount;
    if (s.status() != QDataStream::Ok || clusterCount < 0 || clusterCount > (1 << 24)) return false;
    if (clusterCount == 0) return true;
    qint32 recordSize = 0;
    s >> recordSize;
    const QVector<float> expected = atomdag::configRecord();
    if (s.status() != QDataStream::Ok || recordSize != expected.size()) return false;
    for (qint32 i = 0; i < recordSize; ++i) {
        float f = 0.0f;
        s >> f;
        if (f != expected.at(i)) return false;
    }
    qint32 groupCount = 0;
    s >> groupCount;
    if (s.status() != QDataStream::Ok || groupCount < 1 || groupCount > clusterCount) return false;
    const auto finite = [](float f) { return std::isfinite(f); };
    out->groups.resize(groupCount);
    for (MeshClusterDag::Group &g : out->groups) {
        s >> g.depth >> g.centre[0] >> g.centre[1] >> g.centre[2] >> g.radius >> g.error >> g.estimate;
        if (s.status() != QDataStream::Ok || g.depth < 0 || !finite(g.centre[0]) ||
            !finite(g.centre[1]) || !finite(g.centre[2]) || !finite(g.radius) || g.radius < 0.0f ||
            !finite(g.error) || !(g.error > 0.0f) || !finite(g.estimate) || g.estimate < 0.0f)
            return false;
    }
    out->clusters.resize(clusterCount);
    for (MeshClusterDag::Cluster &c : out->clusters) {
        s >> c.vertexOffset >> c.triangleOffset >> c.vertexCount >> c.triangleCount >> c.group >>
            c.refined >> c.centre[0] >> c.centre[1] >> c.centre[2] >> c.radius;
        if (s.status() != QDataStream::Ok || c.group < 0 || c.group >= groupCount ||
            c.refined < -1 || c.refined >= groupCount || c.refined == c.group ||
            c.vertexCount == 0 || c.vertexCount > 256 || c.triangleCount == 0 ||
            !finite(c.centre[0]) || !finite(c.centre[1]) || !finite(c.centre[2]) ||
            !finite(c.radius) || c.radius < 0.0f)
            return false;
    }
    QByteArray vertexBytes;
    s >> vertexBytes >> out->triangles;
    if (s.status() != QDataStream::Ok || vertexBytes.size() % int(sizeof(quint32)) != 0 ||
        out->triangles.size() % 3 != 0)
        return false;
    out->vertices.resize(vertexBytes.size() / int(sizeof(quint32)));
    std::memcpy(out->vertices.data(), vertexBytes.constData(), size_t(vertexBytes.size()));
    for (quint32 v : out->vertices) if (int(v) >= vertexCount) return false;
    const quint64 triCount = quint64(out->triangles.size() / 3);
    for (const MeshClusterDag::Cluster &c : out->clusters) {
        if (quint64(c.vertexOffset) + c.vertexCount > quint64(out->vertices.size()) ||
            quint64(c.triangleOffset) + c.triangleCount > triCount)
            return false;
        const uchar *t = reinterpret_cast<const uchar *>(out->triangles.constData()) +
                         size_t(c.triangleOffset) * 3u;
        for (int k = 0; k < int(c.triangleCount) * 3; ++k)
            if (t[k] >= c.vertexCount) return false;
        if (c.refined >= 0 && out->groups.at(c.group).error < out->groups.at(c.refined).error)
            return false;   // not monotone: the cut would not be one cut
    }
    return true;
}

// ---- meshes ----------------------------------------------------------------

void writeMesh(QDataStream &s, const MeshPtr &mesh)
{
    s << qint32(mesh->primitiveMode) << qint32(mesh->usesIndexBuffer ? 1 : 0)
      << qint32(mesh->numVerts) << qint32(mesh->numFaces);
    writeVec3(s, mesh->boundingSphere.pos);
    s << float(mesh->boundingSphere.radius);
    writeVec3(s, mesh->aabb.getMin());
    writeVec3(s, mesh->aabb.getMax());

    const QList<VertexBufferPtr> &vbs = mesh->getVertexBuffers();
    s << qint32(vbs.size());
    for (const VertexBufferPtr &vb : vbs) {
        // Every buffer this pipeline builds carries exactly one attribute
        // (Mesh::addVertexArray) — the format records that shape explicitly so
        // a future multi-attribute buffer fails the read instead of silently
        // losing elements.
        QList<VertexAttribute> attribs = vb->vertexLayout.getAttribs();
        s << qint32(attribs.size());
        for (const VertexAttribute &a : attribs)
            s << qint32(a.usage) << qint32(a.type) << qint32(a.count) << qint32(a.sizeInBytes);
        s << QByteArray(vb->data, vb->dataSize);
    }

    const IndexBufferPtr ib = mesh->getIndexBuffer();
    if (ib && ib->data && ib->dataSize > 0) s << QByteArray(ib->data, ib->dataSize);
    else                                     s << QByteArray();

    writeSkeleton(s, mesh->getSkeleton());

    // ATOM stage 1's trailing block (format v7). `levelCount` is the number of
    // levels ABOVE level 0 — zero for every mesh that has no chain, which is
    // most of them, and costs four bytes. Each level is {indexCount, error,
    // indices}; the indices name the vertex buffers written above, unchanged.
    // (format v12: each level gains its MEASURED BOUND beside the simplifier's
    // error — the length every consumer reads. Both are written so the bake can
    // be asked what the simplifier claimed as well as what the surface measures.)
    const int levels = std::min(std::min(mesh->lodIndices.size(), mesh->lodErrors.size()),
                                mesh->lodBounds.size());
    s << qint32(levels);
    for (int i = 0; i < levels; ++i) {
        const QVector<quint32> &idx = mesh->lodIndices.at(i);
        s << qint32(idx.size()) << float(mesh->lodErrors.at(i)) << float(mesh->lodBounds.at(i));
        s << QByteArray(reinterpret_cast<const char *>(idx.constData()),
                        idx.size() * int(sizeof(quint32)));
    }

    // SURFACE-CACHE phase 1's trailing block (format v10). `cardCount` is zero
    // for every mesh that gets no cards — a skinned one, a line mesh, a mesh
    // with no area — which costs four bytes and one float, and is the honest
    // answer rather than a fabricated box.
    s << qint32(mesh->cards.size()) << float(mesh->cardCoverage);
    for (const MeshCard &card : mesh->cards) {
        s << quint8(card.axis) << quint8(card.lodLevel);
        writeVec3(s, card.origin);
        s << float(card.halfU) << float(card.halfV) << float(card.halfDepth)
          << float(card.coverage);
    }

    // ATOM stage 2's trailing block (format v13) — the CLUSTER DAG. A zero
    // cluster count for every mesh that gets none (skinned, not triangles, under
    // two leaves of triangles), which costs four bytes.
    writeClusterDag(s, mesh->clusterDag);
}

MeshPtr readMesh(QDataStream &s, bool *okOut)
{
    qint32 primitive = 0, usesIndex = 0, numVerts = 0, numFaces = 0;
    s >> primitive >> usesIndex >> numVerts >> numFaces;
    if (primitive < 0 || primitive > int(PrimitiveMode::LineStrip)) { *okOut = false; return MeshPtr(); }

    auto mesh = MeshPtr(new Mesh());
    mesh->setPrimitiveMode(PrimitiveMode(primitive));
    mesh->usesIndexBuffer = usesIndex != 0;
    mesh->numVerts = numVerts;
    mesh->numFaces = numFaces;

    mesh->boundingSphere.pos = readVec3(s);
    float radius = 0.0f;
    s >> radius;
    mesh->boundingSphere.radius = radius;
    const iris::Vec3 aabbMin = readVec3(s);
    const iris::Vec3 aabbMax = readVec3(s);
    AABB box;
    box.merge(aabbMin);
    box.merge(aabbMax);
    mesh->aabb = box;

    qint32 vbCount = 0;
    s >> vbCount;
    if (vbCount < 0 || vbCount > 32) { *okOut = false; return MeshPtr(); }
    const float *positions = nullptr;
    int positionFloats = 0;
    for (qint32 i = 0; i < vbCount; ++i) {
        qint32 attribCount = 0;
        s >> attribCount;
        if (attribCount != 1) { *okOut = false; return MeshPtr(); }
        qint32 usage = 0, type = 0, count = 0, sizeInBytes = 0;
        s >> usage >> type >> count >> sizeInBytes;
        if (usage < 0 || usage >= int(VertexAttribUsage::Count)) { *okOut = false; return MeshPtr(); }
        QByteArray data;
        s >> data;
        if (s.status() != QDataStream::Ok) { *okOut = false; return MeshPtr(); }
        VertexLayout layout;
        layout.addAttrib(VertexAttribUsage(usage), type, count, sizeInBytes);
        auto vb = VertexBuffer::create(layout);
        vb->setData(const_cast<char *>(data.constData()), unsigned(data.size()));
        mesh->addVertexBuffer(vb);
        if (VertexAttribUsage(usage) == VertexAttribUsage::Position) {
            positions = reinterpret_cast<const float *>(vb->data);
            positionFloats = vb->dataSize / int(sizeof(float));
        }
    }

    QByteArray indexBytes;
    s >> indexBytes;
    if (s.status() != QDataStream::Ok) { *okOut = false; return MeshPtr(); }
    if (!indexBytes.isEmpty()) {
        auto ib = IndexBuffer::create();
        ib->setData(const_cast<char *>(indexBytes.constData()), unsigned(indexBytes.size()));
        mesh->setIndexBuffer(ib);
    }

    auto skel = readSkeleton(s, okOut);
    if (!*okOut) return MeshPtr();
    if (!skel.isNull()) mesh->setSkeleton(skel);

    // ATOM stage 1's trailing block (format v7).
    qint32 levels = 0;
    s >> levels;
    if (s.status() != QDataStream::Ok || levels < 0 || levels > 32) { *okOut = false; return MeshPtr(); }
    const int vertexCount = positionFloats / 3;
    float previousBound = 0.0f;
    for (qint32 i = 0; i < levels; ++i) {
        qint32 indexCount = 0; float error = 0.0f; float bound = 0.0f;
        s >> indexCount >> error >> bound;
        QByteArray levelBytes;
        s >> levelBytes;
        if (s.status() != QDataStream::Ok || indexCount < 3 || indexCount % 3 != 0 ||
            levelBytes.size() != indexCount * int(sizeof(quint32))) { *okOut = false; return MeshPtr(); }
        // THE BOUND IS THE SELECTION CURRENCY, so a blob that is not a positive
        // finite non-decreasing length is refused rather than read: the one rule
        // walks the array and stops at the first level it cannot afford, which is
        // only the right answer for a sorted array (see Mesh::lodBounds).
        if (!std::isfinite(error) || error < 0.0f || !std::isfinite(bound) ||
            !(bound > 0.0f) || bound < previousBound) { *okOut = false; return MeshPtr(); }
        previousBound = bound;
        QVector<quint32> idx(indexCount);
        std::memcpy(idx.data(), levelBytes.constData(), size_t(levelBytes.size()));
        // A level that names a vertex the mesh does not have would draw
        // garbage the moment the camera backs off — refuse the blob instead.
        for (quint32 v : idx) if (int(v) >= vertexCount) { *okOut = false; return MeshPtr(); }
        mesh->lodIndices.append(idx);
        mesh->lodErrors.append(error);
        mesh->lodBounds.append(bound);
    }

    // SURFACE-CACHE phase 1's trailing block (format v10). Every field is
    // checked: a card whose axis is not one of the six, whose rectangle is not
    // a positive finite size, whose LOD level names a level the mesh does not
    // carry, or whose coverage is not a fraction, would be captured into an
    // atlas by phase 2 and read at a ray hit by phase 4 — refuse the blob and
    // parse instead, which is what every other malformed field here does.
    qint32 cardCount = 0; float cardCoverage = 0.0f;
    s >> cardCount >> cardCoverage;
    if (s.status() != QDataStream::Ok || cardCount < 0 || cardCount > kMaxCardsCeiling
        || !std::isfinite(cardCoverage) || cardCoverage < 0.0f || cardCoverage > 1.0f) {
        *okOut = false; return MeshPtr();
    }
    for (qint32 i = 0; i < cardCount; ++i) {
        quint8 axis = 0, lodLevel = 0;
        s >> axis >> lodLevel;
        MeshCard card;
        card.axis = axis;
        card.lodLevel = lodLevel;
        card.origin = readVec3(s);
        s >> card.halfU >> card.halfV >> card.halfDepth >> card.coverage;
        if (s.status() != QDataStream::Ok || axis >= MeshCard::kAxisCount
            || int(lodLevel) > mesh->lodIndices.size()
            || !std::isfinite(card.origin.x()) || !std::isfinite(card.origin.y())
            || !std::isfinite(card.origin.z())
            || !(card.halfU > 0.0f) || !(card.halfV > 0.0f) || !(card.halfDepth > 0.0f)
            || !std::isfinite(card.halfU) || !std::isfinite(card.halfV)
            || !std::isfinite(card.halfDepth)
            || !std::isfinite(card.coverage) || card.coverage < 0.0f || card.coverage > 1.0f) {
            *okOut = false; return MeshPtr();
        }
        mesh->cards.append(card);
    }
    mesh->cardCoverage = cardCount > 0 ? cardCoverage : 0.0f;

    // ATOM stage 2's trailing block (format v13) — the CLUSTER DAG.
    if (!readClusterDag(s, vertexCount, &mesh->clusterDag)) { *okOut = false; return MeshPtr(); }

    // THE PICKING MESH IS REBUILT, NOT STORED. It is positions + indices with
    // one cross product per triangle — cheaper to recompute than to read, and
    // recomputing is what makes it impossible for a bake to hand picking a
    // geometry the renderer does not have. ITS SPATIAL INDEX TOO (SPEED-CPU,
    // perf audit P1): built here, on the reading thread (the prewarm worker on
    // an open), so the first pick of a 233k-triangle scan is already a
    // tree walk and not the 20 ms loop over every triangle.
    mesh->triMesh = new TriMesh();
    if (positions && !indexBytes.isEmpty()) {
        const unsigned *idx = reinterpret_cast<const unsigned *>(indexBytes.constData());
        const int idxCount = indexBytes.size() / int(sizeof(unsigned));
        const int vertexCount = positionFloats / 3;
        mesh->triMesh->triangles.reserve(idxCount / 3);
        for (int i = 0; i + 2 < idxCount; i += 3) {
            const unsigned a = idx[i], b = idx[i + 1], c = idx[i + 2];
            if (int(a) >= vertexCount || int(b) >= vertexCount || int(c) >= vertexCount) {
                *okOut = false;
                return MeshPtr();
            }
            mesh->triMesh->addTriangle(
                iris::Vec3(positions[a * 3], positions[a * 3 + 1], positions[a * 3 + 2]),
                iris::Vec3(positions[b * 3], positions[b * 3 + 1], positions[b * 3 + 2]),
                iris::Vec3(positions[c * 3], positions[c * 3 + 1], positions[c * 3 + 2]));
        }
        mesh->triMesh->buildIndex();
    }
    return mesh;
}

// ---- animation clips -------------------------------------------------------

template <typename K, typename W>
void writeKeys(QDataStream &s, const K *frame, W writeValue)
{
    s << qint32(frame->keys.size());
    for (const auto *key : frame->keys) {
        writeValue(s, key->value);
        s << double(key->time);
    }
}

void writeAnimations(QDataStream &s, const QMap<QString, SkeletalAnimationPtr> &anims)
{
    s << qint32(anims.size());
    // QMap iterates in key order — the same order on every machine, which is
    // what makes the blob deterministic.
    for (auto it = anims.constBegin(); it != anims.constEnd(); ++it) {
        s << it.key();
        const SkeletalAnimationPtr &anim = it.value();
        s << anim->name;
        s << double(anim->declaredLength);
        s << qint32(anim->boneAnimations.size());
        for (auto b = anim->boneAnimations.constBegin(); b != anim->boneAnimations.constEnd(); ++b) {
            s << b.key();
            writeKeys(s, b.value()->posKeys.data(),
                      [](QDataStream &st, const iris::Vec3 &v) { writeVec3(st, v); });
            writeKeys(s, b.value()->rotKeys.data(),
                      [](QDataStream &st, const iris::Quat &q) { writeQuat(st, q); });
            writeKeys(s, b.value()->scaleKeys.data(),
                      [](QDataStream &st, const iris::Vec3 &v) { writeVec3(st, v); });
        }
    }
}

bool readAnimations(QDataStream &s, const QString &source,
                    QMap<QString, SkeletalAnimationPtr> &out)
{
    qint32 count = 0;
    s >> count;
    if (count < 0 || count > 100000) return false;
    for (qint32 i = 0; i < count; ++i) {
        QString key, name;
        s >> key >> name;
        double declaredLength = 0.0;
        s >> declaredLength;
        qint32 boneCount = 0;
        s >> boneCount;
        if (s.status() != QDataStream::Ok || boneCount < 0 || boneCount > 1000000) return false;
        if (!(declaredLength >= 0.0) || declaredLength > 1.0e7) return false;
        auto anim = SkeletalAnimation::create();
        anim->name = name;
        anim->source = source;
        anim->declaredLength = float(declaredLength);
        for (qint32 b = 0; b < boneCount; ++b) {
            QString boneName;
            s >> boneName;
            auto *boneAnim = new BoneAnimation();
            const auto readTrack = [&](int kind) -> bool {
                qint32 keyCount = 0;
                s >> keyCount;
                if (s.status() != QDataStream::Ok || keyCount < 0 || keyCount > 10000000) return false;
                for (qint32 k = 0; k < keyCount; ++k) {
                    double time = 0.0;
                    if (kind == 1) {
                        const iris::Quat q = readQuat(s);
                        s >> time;
                        boneAnim->rotKeys->addKey(q, time);
                    } else {
                        const iris::Vec3 v = readVec3(s);
                        s >> time;
                        (kind == 0 ? boneAnim->posKeys : boneAnim->scaleKeys)->addKey(v, time);
                    }
                }
                return s.status() == QDataStream::Ok;
            };
            if (!readTrack(0) || !readTrack(1) || !readTrack(2)) { delete boneAnim; return false; }
            anim->addBoneAnimation(boneName, boneAnim);
        }
        out.insert(key, anim);
    }
    return s.status() == QDataStream::Ok;
}

// ---- node tree -------------------------------------------------------------

void writeNode(QDataStream &s, const BakedNode &n)
{
    s << n.name;
    writeVec3(s, n.pos);
    writeVec3(s, n.scale);
    writeQuat(s, n.rot);
    s << qint32(n.isMeshNode ? 1 : 0) << qint32(n.meshIndex) << qint32(n.materialIndex);
    s << qint32(n.meshChildCount);
    s << qint32(n.children.size());
    for (const BakedNode &child : n.children) writeNode(s, child);
}

bool readNode(QDataStream &s, BakedNode &n, int depth)
{
    if (depth > 512) return false;   // a hostile blob must not blow the stack
    s >> n.name;
    n.pos = readVec3(s);
    n.scale = readVec3(s);
    n.rot = readQuat(s);
    qint32 isMesh = 0, meshIndex = -1, materialIndex = -1, meshChildren = 0, childCount = 0;
    s >> isMesh >> meshIndex >> materialIndex >> meshChildren >> childCount;
    if (s.status() != QDataStream::Ok || childCount < 0 || childCount > 1000000) return false;
    if (meshChildren < 0 || meshChildren > childCount) return false;
    n.isMeshNode = isMesh != 0;
    n.meshIndex = meshIndex;
    n.materialIndex = materialIndex;
    n.meshChildCount = meshChildren;
    n.children.resize(childCount);
    for (qint32 i = 0; i < childCount; ++i)
        if (!readNode(s, n.children[i], depth + 1)) return false;
    return true;
}

// ---- plain facts (the describe block, the clip bake) ------------------------
//
// A DOUBLE IS WRITTEN AS ITS BITS. The stream is pinned to single precision (the
// geometry is float), and a clip's length or a file's unit declaration rounded
// to a float would make the backfill disagree with the import's own numbers.

void writeF64(QDataStream &s, double v)
{
    quint64 bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "double is 64-bit");
    std::memcpy(&bits, &v, sizeof(bits));
    s << bits;
}

double readF64(QDataStream &s)
{
    quint64 bits = 0;
    s >> bits;
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

bool readStrings(QDataStream &s, QStringList &out)
{
    qint32 n = 0;
    s >> n;
    if (s.status() != QDataStream::Ok || n < 0 || n > 10000000) return false;
    out.clear();
    out.reserve(n);
    for (qint32 i = 0; i < n; ++i) {
        QString v;
        s >> v;
        out.append(v);
    }
    return s.status() == QDataStream::Ok;
}

void writeStrings(QDataStream &s, const QStringList &list)
{
    s << qint32(list.size());
    for (const QString &v : list) s << v;
}

void writeDescribe(QDataStream &s, const ModelSceneInfo &d)
{
    s << qint32(d.parsed ? 1 : 0);
    s << qint64(d.vertices) << qint64(d.triangles) << qint32(d.meshes) << qint32(d.materials);
    writeStrings(s, d.textureReferences);
    s << qint32(d.embeddedTextures);
    writeStrings(s, d.boneNames);
    writeStrings(s, d.nodeNames);
    s << qint32(d.animations.size());
    for (const ModelSceneInfo::Animation &a : d.animations) {
        s << a.name;
        writeF64(s, a.ticksPerSecond);
        writeF64(s, a.durationTicks);
        writeF64(s, a.lengthSeconds);
        writeStrings(s, a.channelNames);
    }
    writeF64(s, d.extentX);
    writeF64(s, d.extentY);
    writeF64(s, d.extentZ);
    s << qint32(d.extentValid ? 1 : 0);
    writeF64(s, d.declaredUnitScale);
}

bool readDescribe(QDataStream &s, ModelSceneInfo &d)
{
    qint32 parsed = 0, meshes = 0, materials = 0, embedded = 0, animCount = 0;
    qint64 vertices = 0, triangles = 0;
    s >> parsed >> vertices >> triangles >> meshes >> materials;
    if (!readStrings(s, d.textureReferences)) return false;
    s >> embedded;
    if (!readStrings(s, d.boneNames) || !readStrings(s, d.nodeNames)) return false;
    s >> animCount;
    if (s.status() != QDataStream::Ok || animCount < 0 || animCount > 100000) return false;
    d.parsed = parsed != 0;
    d.vertices = vertices;
    d.triangles = triangles;
    d.meshes = meshes;
    d.materials = materials;
    d.embeddedTextures = embedded;
    d.animations.clear();
    for (qint32 i = 0; i < animCount; ++i) {
        ModelSceneInfo::Animation a;
        s >> a.name;
        a.ticksPerSecond = readF64(s);
        a.durationTicks = readF64(s);
        a.lengthSeconds = readF64(s);
        if (!readStrings(s, a.channelNames)) return false;
        d.animations.append(a);
    }
    d.extentX = readF64(s);
    d.extentY = readF64(s);
    d.extentZ = readF64(s);
    qint32 extentValid = 0;
    s >> extentValid;
    d.extentValid = extentValid != 0;
    d.declaredUnitScale = readF64(s);
    return s.status() == QDataStream::Ok;
}

void writeClipInfo(QDataStream &s, const ClipFileInfo &info)
{
    s << qint32(info.parsed ? 1 : 0) << qint32(info.meshes) << qint32(info.animations);
    s << qint32(info.clips.size());
    for (const ClipFileInfo::Clip &c : info.clips) {
        s << c.name;
        writeF64(s, c.ticksPerSecond);
        writeF64(s, c.durationTicks);
        writeF64(s, c.lengthSeconds);
        writeStrings(s, c.channelNames);
    }
    writeStrings(s, info.nodeNames);
    s << qint32(info.nodeParents.size());
    for (int parent : info.nodeParents) s << qint32(parent);
    s << qint32(info.poses.size());
    for (const ClipFileInfo::Pose &pose : info.poses) {
        s << qint32(pose.positions.size());
        for (const QVector3D &p : pose.positions) s << float(p.x()) << float(p.y()) << float(p.z());
    }
}

bool readClipInfo(QDataStream &s, ClipFileInfo &info)
{
    qint32 parsed = 0, meshes = 0, animations = 0, clipCount = 0;
    s >> parsed >> meshes >> animations >> clipCount;
    if (s.status() != QDataStream::Ok || clipCount < 0 || clipCount > 100000) return false;
    info.parsed = parsed != 0;
    info.meshes = meshes;
    info.animations = animations;
    for (qint32 i = 0; i < clipCount; ++i) {
        ClipFileInfo::Clip c;
        s >> c.name;
        c.ticksPerSecond = readF64(s);
        c.durationTicks = readF64(s);
        c.lengthSeconds = readF64(s);
        if (!readStrings(s, c.channelNames)) return false;
        info.clips.append(c);
    }
    if (!readStrings(s, info.nodeNames)) return false;
    qint32 parents = 0;
    s >> parents;
    if (s.status() != QDataStream::Ok || parents < 0 || parents > 10000000) return false;
    info.nodeParents.reserve(parents);
    for (qint32 i = 0; i < parents; ++i) {
        qint32 parent = -1;
        s >> parent;
        info.nodeParents.append(parent);
    }
    qint32 poseCount = 0;
    s >> poseCount;
    if (s.status() != QDataStream::Ok || poseCount < 0 || poseCount > 64) return false;
    for (qint32 i = 0; i < poseCount; ++i) {
        qint32 n = 0;
        s >> n;
        if (s.status() != QDataStream::Ok || n < 0 || n > 10000000) return false;
        ClipFileInfo::Pose pose;
        pose.positions.reserve(n);
        for (qint32 j = 0; j < n; ++j) {
            float x = 0, y = 0, z = 0;
            s >> x >> y >> z;
            pose.positions.append(QVector3D(x, y, z));
        }
        info.poses.append(pose);
    }
    return s.status() == QDataStream::Ok;
}

}   // namespace

// ---------------------------------------------------------------------------

int MeshBake::formatVersion() { return kFormatVersion; }

QString MeshBake::producerId()
{
    return QStringLiteral("v%1|%2|assimp%3|flags%4")
        .arg(kFormatVersion)
        .arg(QLatin1String(JAHSHAKA_MESH_BAKE_PRODUCER_ID))
        .arg(ModelSceneInfo::importerVersion())
        .arg(quint64(iris::ImportFlags::Canonical));
}

QString MeshBake::producerHash()
{
    return QString::fromLatin1(JAHSHAKA_MESH_BAKE_PRODUCER_ID);
}

QStringList MeshBake::hashedSources()
{
    const QString list = QString::fromLatin1(JAHSHAKA_MESH_BAKE_HASHED_SOURCES);
    if (list.isEmpty()) return QStringList();
    return list.split(QLatin1Char('|'), Qt::SkipEmptyParts);
}

QString MeshBake::producerHashOf(const QStringList &absolutePaths)
{
    // EXACTLY irisgl/CMakeLists.txt's algorithm, so the suite can prove the two
    // agree on the real tree and then reason about scratch copies: each file's
    // lowercase hex SHA256, each followed by ';', concatenated in list order,
    // and the SHA256 of that ASCII string. An unreadable file yields an empty
    // result rather than a hash that silently means "one file less".
    QByteArray blob;
    for (const QString &path : absolutePaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return QString();
        QCryptographicHash h(QCryptographicHash::Sha256);
        if (!h.addData(&file)) return QString();
        blob += h.result().toHex();
        blob += ';';
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(blob, QCryptographicHash::Sha256).toHex());
}

QString MeshBake::fingerprintFor(const QString &sourceOid, const QString &settingsHash)
{
    if (sourceOid.isEmpty()) return QString();
    const QString settings = settingsHash.isEmpty() ? ImportSettings::identityHash() : settingsHash;
    const QByteArray key = (producerId() + QLatin1Char('|') + sourceOid
                            + QLatin1Char('|') + settings).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
}

QString MeshBake::fileNameFor(const QString &sourceOid, const QString &settingsHash)
{
    const QString settings = settingsHash.isEmpty() ? ImportSettings::identityHash() : settingsHash;
    return QStringLiteral("%1-%2.jmb").arg(sourceOid.left(16), settings);
}

QString MeshBake::casRole() { return QStringLiteral("bake"); }

// ---- build -----------------------------------------------------------------

namespace
{


/// The bake's node walk — the exact rules `_buildScene` applies, recorded as
/// data instead of as SceneNodes. Kept beside loadAsSceneFragment in review
/// terms: any change to one is a change to both, and the round-trip suite
/// (tests/meshbake) compares the two trees node for node.
BakedNode bakeNode(const aiScene *scene, const aiNode *node, QVector<int> &materialFor)
{
    BakedNode out;
    if (node->mNumMeshes == 1) {
        out.isMeshNode = true;
        const unsigned index = node->mMeshes[0];
        const aiMesh *mesh = scene->mMeshes[index];
        if (mesh->HasPositions()) {
            out.meshIndex = int(index);
            out.materialIndex = materialFor.value(int(index), -1);
            out.name = QString(mesh->mName.C_Str());
        }
    } else {
        out.name = QString(node->mName.C_Str());
        for (unsigned i = 0; i < node->mNumMeshes; ++i) {
            const unsigned index = node->mMeshes[i];
            BakedNode child;
            child.isMeshNode = true;
            child.meshIndex = int(index);
            child.materialIndex = materialFor.value(int(index), -1);
            child.name = QString(scene->mMeshes[index]->mName.C_Str());
            out.children.append(child);
        }
        out.meshChildCount = out.children.size();
    }

    aiVector3D pos, scale;
    aiQuaternion rot;
    aiMatrix4x4 transform = node->mTransformation;
    transform.Decompose(scale, rot, pos);
    out.pos = iris::Vec3(pos.x, pos.y, pos.z);
    out.scale = iris::Vec3(scale.x, scale.y, scale.z);
    out.rot = iris::Quat(rot.w, rot.x, rot.y, rot.z);

    for (unsigned i = 0; i < node->mNumChildren; ++i)
        out.children.append(bakeNode(scene, node->mChildren[i], materialFor));
    return out;
}

/// The single-mesh shortcut's transform: the accumulated (root -> node)
/// transform of the first aiNode referencing mesh 0 — meshnode.cpp's
/// _findMeshNodeTransform, over the same convention (child * parent).
bool findMeshNodeTransform(const aiNode *node, unsigned meshIndex,
                           const aiMatrix4x4 &parent, aiMatrix4x4 &out)
{
    // parent * child: aiMatrix4x4 multiplies COLUMN vectors, so the ancestor
    // goes on the left (IMPORT-1: it read child * parent, which only agreed
    // with ModelSceneInfo's own walk while every ancestor was identity — and
    // an import transform pre-multiplied onto the root is not).
    const aiMatrix4x4 global = parent * node->mTransformation;
    for (unsigned i = 0; i < node->mNumMeshes; ++i)
        if (node->mMeshes[i] == meshIndex) { out = global; return true; }
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        if (findMeshNodeTransform(node->mChildren[i], meshIndex, global, out)) return true;
    return false;
}



// ---- ATOM stage 1: the automatic LOD chain ---------------------------------
//
// SPECS/NANITE_SPEC.md §7. The artist authors nothing: the machine simplifies
// each STATIC mesh a few times at import, records each level's index list and
// its simplifier ERROR (>= the geometric error), and the engine picks a level
// per object per frame from that error (irisgl/engine/src/OgreMesh.cpp,
// the `jah_world_error` LodStrategy + Types.h::lodLevelForWorldError).
//
// THE THREE RULES THIS OBEYS, each with its reason:
//
//  1. ONE VERTEX BUFFER, N INDEX BUFFERS. meshopt_simplify REMOVES vertices, it
//     never creates them, and the output indexes the ORIGINAL vertex buffer.
//     meshopt_optimizeVertexFetch would compact each level — and REMAP the
//     vertices, which changes the VAO's vertex-buffer set, which changes the
//     vaoName (OgreVulkanVaoManager::findVao matches on {opType, indexBufferVbo,
//     indexType, vertexBuffers}), which splits the auto-instancing merge in
//     RenderQueue::render into one draw command per level. The unused-vertex
//     waste at the coarse levels is accepted; the draw count is not negotiable.
//
//  2. THE ERROR IS A LENGTH IN MESH UNITS, not a ratio. meshopt_simplify's
//     result_error is relative to the mesh extent unless meshopt_SimplifyErrorAbsolute
//     is passed; we pass it, so the number that reaches the bake is the one the
//     projection formula divides by a view distance
//     (thirdparty/meshoptimizer-clusterlod/clusterlod.h:94-97). That is the
//     scale-invariant currency the Nanite paper insists on
//     (SPECS/research/NANITE_2021_PDF_AUDIT_2026-09-15.md §1: Epic normalise the
//     quadric by the average triangle area for exactly this reason — an error
//     that is not comparable across meshes cannot drive one global threshold).
//     Errors ACCUMULATE monotonically down the chain by clusterlod.h's own rule
//     at its default config: error_i = max(error_{i-1}, step_error_i).
//
//  3. THE ATTRIBUTE METRIC IS NORMALS AND UVs, and the UV weight is DERIVED,
//     not chosen: `0.5 * extent / uvRange`. Normals ride at 0.5, the weight
//     meshoptimizer's own reference code uses (demo/main.cpp:316,
//     demo/nanite.cpp:88). A UV delta is not a length, so a FIXED uv weight is
//     not scale-invariant — a mesh with tiled UVs (0..20) would be penalised
//     twenty times harder than the same mesh at 0..1; dividing by the mesh's
//     own UV range and multiplying by its extent makes "slide the texture
//     across the whole UV range" cost the same as "move the surface by the
//     whole model", on every mesh, in world units.
//
//  4. PERMISSIVE, AND THE SEAMS ARE PAID FOR IN THE ERROR RATHER THAN LOCKED.
//     MEASURED, not chosen. Without meshopt_SimplifyPermissive the simplifier
//     never collapses across an attribute discontinuity, and a split asset is
//     discontinuous almost everywhere: on the Matcaps sample's Stanford Dragon
//     (89,067 verts over 34,110 distinct POSITIONS — the fan-out is UV splits;
//     68,220 triangles) plain simplification STALLED at 68% of the triangles on
//     the first level and produced no second level, and on the gizmo gear it
//     blew an error of 36% of the mesh extent on the first level. With
//     Permissive the same dragon runs the full chain 50/25/12.5/6.2% at a final
//     error of 1.2% of its extent. Upstream marks the flag experimental;
//     clusterlod.h — the reference implementation of the cluster DAG stage 2
//     builds on — turns it ON in its DEFAULT config (`simplify_permissive`).
//
//     clusterlod.h pairs it with an attribute_protect_mask that LOCKS every UV
//     seam vertex. We measured that too, and it is the wrong trade HERE: on the
//     dragon 29,151 of 89,067 vertices (33%) are UV-seam vertices, so locking
//     them is locking the mesh — the chain died at level 2 with an error of
//     0.57 (12% of the extent) instead of running four levels at 0.059. Locking
//     is right for a CLUSTER (stage 2 must keep a group's boundary watertight
//     against its neighbours); for a whole object the seam is interior to the
//     draw and the honest cost of collapsing it is texture stretch — which the
//     UV term in the metric CHARGES, as error, which pushes the level further
//     away. Measured: +17% error per level versus normals alone, same chain.
//
// SKINNED MESHES GET NO CHAIN (stage 1 ships static only): the CPU-skinning
// path writes poses into mVao[VpNormal][0]'s buffer alone
// (irisgl/engine/src/OgreMesh.cpp), and blend indices/weights surviving
// simplification is its own gate. A mesh with a skeleton is skipped here.

namespace lodchain {

/// The knobs, in one place, with the reason each exists. These are BAKE INPUTS:
/// meshbake.cpp is hashed into the producer id, so editing any of them
/// re-bakes every library by itself.
/// THE CHAIN ENDS AT A TRIANGLE FLOOR, NOT AT A LEVEL COUNT (ATOM-3 A6, the
/// render audit's A6). `kMaxLevels = 4` ended every chain at 1/16 of the
/// authored triangles whatever the asset was, so a 1 M-triangle model's
/// coarsest level was 62 k triangles — not a far-field proxy, just a slightly
/// smaller model (Nanite's root cluster is ~128 triangles, which is exactly
/// `kMinTriangles` below). The loop now halves until one of the three rules
/// that MEAN something stops it: the triangle floor, a level that could not
/// shed 15 % (topology), or an error past 5 % of the mesh extent. This ceiling
/// is the uint8 in `MovableObject::mCurrentMeshLod` and nothing else — at
/// `kRatio` 0.5 a 254-level chain would need 2^254 triangles, so it can never
/// be the rule that fires.
constexpr int   kMaxLevels     = 254;
constexpr float kRatio         = 0.5f;   ///< each level targets half the previous triangle count (the paper's step).
constexpr int   kMinTriangles  = 128;    ///< THE FLOOR: below this a level saves nothing worth a buffer — and is clusterlod's own leaf size, and Nanite's root.
/// kAcceptRatio — A LEVEL THAT COULD NOT SHED 15 % IS NOT WORTH A BUFFER, and the
/// 15 % is arithmetic rather than taste. A stored level costs an index buffer, a
/// VertexArrayObject, a threshold in the value array `lodSet` binary-searches every
/// frame, and a SWITCH — a visible change of silhouette. What it buys is the
/// triangles it sheds. The chain's own step is `kRatio` 0.5, so a level that
/// delivers only 0.85 of the previous count has delivered 30 % of the reduction it
/// was asked for, and the next level will be asked to halve THAT: the loop is
/// already at the point where topology, not the error budget, is deciding the
/// result. Below about 0.85 the remaining chain is levels that each cost a switch
/// for a tenth of a halving. It is the same number clusterlod.h's reference
/// configuration stops at, for the same reason.
constexpr float kAcceptRatio   = 0.85f;
/// kMaxRelError — THE SIMPLIFIER'S OWN BUDGET PER LEVEL, and since ATOM-BAKE-1 it is
/// explicitly a CHAIN-SHAPE knob and not the selection currency (`lodBounds` is that,
/// and the safety net on it is `kMaxRelBound` below).
///
/// WHY IT STAYS ON THE QUADRIC even though the quadric is not a bound: this number
/// decides how far the loop is allowed to GO, and it has to be a quantity the
/// simplifier can be steered by BEFORE anything is measured — the measurement
/// happens after a level exists. 5 % of the mesh's largest extent is the point at
/// which a simplified whole object stops being a stand-in for itself and becomes a
/// blob: at 5 % a 2 m sphere may deviate 10 cm, which at the view's one-pixel budget
/// is a level taken only past ~200 m. Beyond that the chain is producing levels
/// nothing will ever select at a distance anything renders at.
///
/// AND THE MEASUREMENT THAT SAYS IT IS THE RIGHT ORDER: on the shipped meshes the
/// cap is NOT what stops the chain — the triangle floor (`kMinTriangles`) is, on
/// every one of them (sphere 960 -> 240, capsule 1024 -> 128, torus 1536 -> 192,
/// hp_sphere 3072 -> 192). A knob that never fires on real content is not tuned
/// against it; it is a ceiling, which is what it is here for.
constexpr float kMaxRelError   = 0.05f;
constexpr float kNormalWeight  = 0.5f;   ///< meshoptimizer's own reference weight for unit normals.
constexpr float kUvWeight      = 0.5f;   ///< the same relative priority, times extent/uvRange (see 3 above).

// ---- THE MEASURED BOUND (ATOM P1's AT-A5) ---------------------------------
//
// WHAT WAS WRONG. `lodErrors[k]` is meshoptimizer's `result_error` under
// `meshopt_SimplifyErrorAbsolute`, and it is an ESTIMATE, not a bound — verified
// by reading the simplifier rather than by reading the promise:
//
//   * the contract says so: "result_error ... will contain the resulting
//     (relative/absolute) error after simplification" (meshoptimizer.h:511), and
//     `meshopt_simplify`'s own header calls the metric an approximation;
//   * mechanically it is a RUNNING MAX over per-collapse quadric errors
//     (simplifier.cpp:1622, `result_error = max(result_error, c.error)`), and
//     `quadricError` (simplifier.cpp:776) is an AREA-WEIGHTED MEAN of squared
//     plane distances normalised by the accumulated weight (`* 1/Q.w`) — a mean
//     over merged planes, not a supremum over the surface;
//   * a max over passes does not compose: a vertex collapsed in one pass and
//     again in the next accumulates displacement while only the larger SINGLE
//     step is recorded;
//   * with attributes the number is not even a length — `quadricError`'s
//     attribute overload deliberately does not normalise by `Q.w` and mixes in
//     UV and normal deviation (simplifier.cpp:784-800).
//
// Our own comment in Types.h promised the opposite ("the worst distance a
// level's surface may sit from the authored one"), and FOUR consumers lean on
// that promise: the view's pixel-error rule, the cascade's cell rule, the card's
// texel rule and (P4) the ray tier.
//
// WHAT IS STORED INSTEAD. After a level is accepted the bake MEASURES a
// two-sided sampled distance between level k and LEVEL 0 — a sampled Hausdorff
// estimate — and that is what every consumer reads (`lodBounds`). `lodErrors`
// stays, unread by any consumer, as the diagnostic it always was: "what the
// simplifier said".
//
// BOTH DIRECTIONS, because one of them is blind. d(k -> 0) alone misses a level
// that DELETED a feature: every surviving point can sit on level 0 while a whole
// spike is gone. d(0 -> k) alone misses a level that ADDED surface where there
// was none. The bound is the max of the two, which is what a Hausdorff distance
// is.
//
// THE MARGIN IS THE SAMPLING GAP, and it is the honest part of this. N samples
// of a surface cannot see a deviation whose footprint is smaller than the mean
// sample spacing, so the measured maximum under-states the true one; 1.25 is the
// factor applied for it. It is NOT a safety pad for the method — the method is
// exact per sample (surface::TriangleGrid's query is the brute-force answer) —
// it pays for the sample count alone, which is why it travels with N — and why
// it multiplies the AREA term only (ATOM-LOD-BOUND-1). The removed-vertex term
// is not sampled: every removed vertex is queried exactly, so there is no gap to
// pay for, and multiplying it too made an exact number 25 % worse than itself —
// on the Stanford dragon that alone put level 1's bound at 2.11x the simplifier's
// error (0.0171 against 0.0081) where the measured displacement is 1.69x.
constexpr int   kBoundSamples      = 4096;   ///< samples per level PER DIRECTION.
constexpr int   kBoundBigTriangles = 100000; ///< above this a mesh has more surface than 4096 samples resolve...
constexpr int   kBoundSamplesBig   = 8192;   ///< ...so it gets twice as many.
constexpr float kBoundMargin       = 1.25f;  ///< the sampling gap, above.

/// CELLS PER AXIS OF THE NEAREST-SURFACE GRID, AS A FUNCTION OF THE TRIANGLE
/// COUNT. A fixed 32 was wrong in the expensive direction: the grid's query cost
/// is the triangles it has to test per shell, so at one million triangles a 32^3
/// grid holds ~30 triangles per cell and every one of the tens of thousands of
/// queries walks all of them. Roughly ONE TRIANGLE PER CELL is what makes the
/// shell walk O(1) per query, and that is the cube root of the count. Clamped to
/// [32, 128]: below 32 the grid cannot localise anything, and 128^3 is 2 M cells
/// of index vectors, which is the most a per-mesh scratch structure may cost.
inline int boundGridRes(size_t triangles)
{
    const double cube = std::cbrt(double(std::max<size_t>(triangles, 1)));
    return std::clamp(int(cube), 32, 128);
}

/// AND A FLOOR AT THE ARITHMETIC'S OWN NOISE. A bound is a distance measured
/// between two surfaces stored as 32-bit floats, so a claim finer than that
/// arithmetic can resolve is not a measurement — it is noise with a units label.
///
/// DERIVED, from the case that found it. `ground.obj` is a 100 m FLAT grid: every
/// level of it is exactly coplanar with level 0, so its true geometric deviation
/// is ZERO. The sampler returned 0.000054 to 0.000177 — four to fifteen ULPs at
/// 100 m, where float eps is 1.19e-5 — and an independent sampling eight times as
/// dense returned a value 0.8 % different, because the two were comparing noise.
/// `closestOnTriangle` solves for barycentric coordinates through differences of
/// products, which loses about an order of magnitude of ULPs to cancellation, so
/// the resolution of this whole measurement is ~100 ULPs of the COORDINATE SCALE:
/// 1e-5 of the extent (84 ULPs at 100 m, and float eps is 1.19e-7 relative).
/// Below that the honest answer is "as close as these coordinates can say".
///
/// IT TOUCHES NOTHING THAT IS NOT DEGENERATE. Every curved shipped mesh sits three
/// to four orders of magnitude above its own floor (the sphere's floor is 2e-5
/// against a level-1 bound of 0.032); the floor only ever fires where the true
/// deviation really is zero — which is exactly where a relative margin cannot
/// help, because there is nothing to be a fraction of.
///
/// AND IT MAKES THE ANSWER HONEST DOWNSTREAM rather than optimistic: a consumer
/// dividing by a 5e-5 bound would be told the coarsest floor level is free at any
/// distance on the strength of a number the geometry cannot support. It IS free at
/// any distance — the floor really is flat — and the floored bound says so while
/// remaining a length the format can be trusted about.
constexpr float kBoundFloorRel     = 1e-5f;

/// AND THE ONE SAFETY NET ON THE MEASUREMENT, deliberately generous: a level
/// whose measured surface may sit a QUARTER of the whole object away is not a
/// stand-in for it by any reading, and storing it would hand the far-field a
/// blob that the one rule would still happily select at distance. It is set far
/// above `kMaxRelError` on purpose — the chain's SHAPE stays the simplifier's
/// decision (so measuring the bound did not silently shorten every chain in the
/// library), and this only ever fires on a mesh whose quadric under-states real
/// deviation by 5x or more. ATOM-BAKE-1 measured it firing on 0 of the shipped
/// meshes; if it ever fires it is the interesting thing in the bake log.
constexpr float kMaxRelBound   = 0.25f;

/// kDisplacementBudget / kLockPasses — THE DISPLACEMENT LOCK (`lodchain::build`).
/// A removed vertex may sit at most this many times the level's own simplifier
/// error from the level's surface; one further out is locked (with its fan) and
/// the level is re-simplified, at most `kLockPasses` simplifications per level.
/// Past the pass budget the level keeps what it has and the bound charges it
/// honestly — the lock shapes the level, it never shapes the measurement. A
/// displaced point is a removed vertex OR a point of a dropped facet (F1: a sliver
/// peaks mid-facet); the owner's scan converges in 2-3 passes, one level in 4.
///
/// WHY 2, MEASURED (ATOM-LOD-BOUND-1, spikes/atom-lod-bound-1/): it is the
/// largest factor the shipped smooth content already honours with no lock at all
/// — the Stanford dragon's worst level is 1.69x, the endless plane's 1.54x, so at
/// 2 their chains are byte-for-byte the plain simplifier's — and on the owner's
/// scan it converges in two to three passes with every level at 1.84-1.98x (at
/// 1.5 the dragon takes locks and the scan's third mesh does not converge in the
/// pass budget). The quadric is an area-weighted MEAN of plane distances and is
/// blind to what a scan is made of: planes extend forever, so the tip of a spike
/// collapses onto its own base "on the plane", and a 1-triangle island collapses
/// to nothing at almost no cost. Two is the tolerance on that estimate.
constexpr float kDisplacementBudget = 2.0f;
constexpr int   kLockPasses    = 4;

/// THE ISLANDS OF A MESH, and why the bound needs them (ATOM-LOD-BOUND-1).
///
/// WHAT WAS WRONG. A scan is one big surface plus hundreds of DEBRIS ISLANDS —
/// disconnected components of one to a few dozen triangles floating around it
/// (the owner's greek_temple_scan.glb: 46 / 733 / 1238 components in its three
/// meshes, 42 / 712 / 1199 of them under 100 triangles). `meshopt_simplify`
/// collapses a small island to nothing as readily as any other edge, and the
/// removed-vertex walk then measured each deleted island's vertices against the
/// NEAREST SURVIVING SURFACE — metres away, some unrelated wall. Measured at the
/// scan's level 1: every one of the three meshes' worst removed vertices sat on
/// an island of 1-6 triangles with an extent of 0.05-0.36 m, 1.9-4.0 m from any
/// surface the level kept, so the stored bound was 2.48 / 2.63 / 5.05 m against
/// a simplifier error of 0.024 / 0.030 / 0.044 — 100-115x — and no view distance
/// under 1 km ever afforded level 1.
///
/// THE RULE: THE BOUND MEASURES REPRESENTED SURFACE. A point of level 0 whose
/// component survives in the level is displaced by its distance to the level's
/// surface — the Hausdorff term, unchanged. A point whose component the level
/// DROPPED is content the level no longer shows, and the most a viewer can miss
/// by its absence is the island itself: its distance is capped at its component's
/// extent (the diagonal of its box — no point of it is further than that from any
/// other point of it). The cap is `min(d, extent)` on every level-0 point, which
/// is inert on anything big (the temple's main body is 43 m across; no removed
/// vertex of it sits 43 m from the level) and on every single-component mesh (all
/// the shipped primitives), and exactly the island's size on a deleted island.
///
/// AND THE SIMPLIFIER IS NOT ALLOWED TO MAKE THAT ERROR IN THE FIRST PLACE. A
/// capped bound is honest but still prices the whole chain by its debris: a 0.36 m
/// island dropped at a level whose surface moved 0.024 m makes the level cost
/// 0.36 m. With the cap alone the scan's level-1 bounds were 0.17 / 0.63 / 0.69 m
/// (7-21x the simplifier's error): half of it islands, half slivers and spikes
/// of the big surfaces the quadric under-prices. So the level is re-simplified
/// under THE DISPLACEMENT LOCK (`kDisplacementBudget`, in `build`): every removed
/// vertex displaced past twice the level's own error is locked with its fan, and
/// every component whose erasure would cost more than that keeps its largest
/// triangle. After it the scan's level-1 bounds are 0.047 / 0.059 / 0.088 m — at
/// most 2x the simplifier's error, at the same triangle counts. Whatever the pass
/// budget leaves is charged by the cap, honestly.
///
/// `meshopt_SimplifyPrune` IS NOT THE TOOL — for the chain on the scan (it does not
/// stop the edge collapses from erasing a larger island) and for the DAG (ATOM-6
/// stage 0, measured on the Bistro: it prunes by its own quadric error REGARDLESS
/// OF LOCKS, so pieces on a group's border vanished — 706 groups lost a locked
/// position — and the facades' errors rose x16-x26). THE DAG'S PRUNE IS OURS
/// (import/atomdag/simplify.h, `pruneUnlocked`): after the collapses, a group still
/// above its target removes WHOLE the pieces that hold no locked position, smallest
/// first, within its budget — and the measurement charges each its extent.
///
/// COMPONENTS ARE WELDED BY POSITION (`meshopt_generatePositionRemap`): a scan's
/// UV seams split vertices everywhere, and a component by index would be cut
/// along every seam into pieces that are not islands at all — the same welding
/// the simplifier itself does when it decides what is a border.
using Islands = atomdag::Islands;   // moved to import/atomdag/adjacency.h (ATOM-2): the DAG builder and the chain read one
using atomdag::findIslands;

/// QUERIES PER CHUNK, at least: one nearest-surface query costs microseconds, a
/// chunk hand-out about one, so a chunk of 64 keeps the pool's overhead in the
/// noise without starving 20 threads on a few thousand samples.
constexpr size_t kQueryGrain = 64;

/// THE MAXIMUM OF A DISTANCE OVER [0, n), on the bake's threads — the serial
/// loop `m = max(m, d)` over finite d, starting at 0, and bit-identical to it:
/// each chunk folds its own items in order into its own slot, and the chunks are
/// folded in order after (`bakepool`, rule 2). `distanceOf(i, query)` is one
/// item's distance with the calling thread's query scratch.
template <class F>
float parallelMax(size_t n, surface::Queries &queries, F &&distanceOf)
{
    const bakepool::Range range(n, kQueryGrain);
    std::vector<float> chunkMax(range.count, 0.0f);
    bakepool::forRange(range, int(queries.size()), [&](size_t c, size_t begin, size_t end, int slot) {
        float m = 0.0f;
        for (size_t i = begin; i < end; ++i) {
            const float d = distanceOf(i, queries[size_t(slot)]);
            if (std::isfinite(d)) m = std::max(m, d);
        }
        chunkMax[c] = m;
    });
    float m = 0.0f;
    for (float v : chunkMax) m = std::max(m, v);
    return m;
}

/// THE MEASURED TWO-SIDED DISTANCE between a level `a` and level 0 `b` over ONE
/// vertex buffer (every level of a chain shares the vertices, ATOM rule 1), as its
/// two terms: `areaOut` the sampled one (before the margin) and the return value
/// the exact one over the removed vertices. `gridA` must be the grid of `a`,
/// `gridB` of `b`. `islands` caps every level-0 point at its component's extent
/// (the represented-surface rule above); `worstVertexOut` names the vertex the
/// exact term came from.
///
/// TWO SAMPLE SETS, AND THE SECOND ONE IS THE MAXIMUM — it was added after the
/// acceptance suite caught its absence and then CORRECTED after the lane's audit
/// read what it was actually measuring.
///
///   * AREA SAMPLES, both ways, cover the surfaces evenly: the distance to a
///     union of triangles can peak inside a facet, and only samples see that.
///   * AND THE BASE VERTICES THE LEVEL NO LONGER HAS. `meshopt_simplify` REMOVES
///     vertices without ever moving one (its output is an index list over the
///     ORIGINAL vertex buffer — ATOM rule 1), so the worst displacement of the
///     authored surface sits, almost always, on the vertices level 0 has and level
///     k does not; a vertex level k still USES is a vertex of one of its own
///     triangles, whose distance to level k is exactly zero. This term is EXACT —
///     every removed vertex is queried, no stride — which is why the sampling-gap
///     margin applies to the area term alone (`lodchain::build`).
/// THE SAMPLED TERM: area samples both ways, the level-0 side capped by island.
float areaDistance(const float *positions, int posComps,
                   const std::vector<unsigned> &a, const surface::TriangleGrid &gridA,
                   const std::vector<unsigned> &b, const surface::TriangleGrid &gridB,
                   size_t samples, const Islands *islands, surface::Queries &queries)
{
    float area = 0.0f;
    std::vector<surface::Sample> pts;
    // The level's own surface against level 0: surface the level ADDED. Never
    // capped — a level has no island level 0 lacks.
    if (surface::sample(positions, posComps, a, samples, &pts) > 0.0f)
        area = std::max(area, parallelMax(pts.size(), queries, [&](size_t i, surface::TriangleGrid::Query &q) {
            return gridB.closest(q, pts[i].pos);
        }));
    // Level 0 against the level: surface the level MOVED or DROPPED, capped by
    // the island the sample lies on.
    if (surface::sample(positions, posComps, b, samples, &pts) > 0.0f)
        area = std::max(area, parallelMax(pts.size(), queries, [&](size_t i, surface::TriangleGrid::Query &q) {
            float d = gridA.closest(q, pts[i].pos);
            if (islands) d = std::min(d, islands->capOf[b[size_t(pts[i].tri) * 3]]);
            return d;
        }));
    return area;
}

/// The sorted, unique vertex set of an index list.
std::vector<unsigned> vertexSet(const unsigned *indices, size_t count)
{
    std::vector<unsigned> v(indices, indices + count);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

/// THE EXACT TERM: every vertex `baseSet` (level 0's, sorted unique) has and the
/// level `a` does not, against the level's own surface, island-capped. Returns
/// the maximum; `each` (optional) receives every removed vertex with its distance
/// — the displacement lock's input — and `worstVertexOut` the arg-max.
float removedVertexDistance(const float *positions, int posComps,
                            const std::vector<unsigned> &baseSet,
                            const std::vector<unsigned> &a, const surface::TriangleGrid &gridA,
                            const Islands *islands, surface::Queries &queries,
                            std::vector<std::pair<unsigned, float>> *each = nullptr,
                            unsigned *worstVertexOut = nullptr)
{
    const std::vector<unsigned> kept = vertexSet(a.data(), a.size());
    std::vector<unsigned> removed;
    removed.reserve(baseSet.size());
    std::set_difference(baseSet.begin(), baseSet.end(), kept.begin(), kept.end(),
                        std::back_inserter(removed));
    // Per chunk: its maximum, its FIRST arg-max (strict `>`, as the serial walk),
    // and its list; folded in chunk order — the serial walk's answer exactly.
    struct Chunk { float worst = 0.0f; unsigned vertex = std::numeric_limits<unsigned>::max();
                   std::vector<std::pair<unsigned, float>> each; };
    const bakepool::Range range(removed.size(), kQueryGrain);
    std::vector<Chunk> chunks(range.count);
    bakepool::forRange(range, int(queries.size()), [&](size_t c, size_t begin, size_t end, int slot) {
        Chunk &out = chunks[c];
        if (each) out.each.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) {
            const unsigned v = removed[i];
            const float *p = positions + size_t(v) * size_t(posComps);
            float d = gridA.closest(queries[size_t(slot)], Vec3(p[0], p[1], p[2]));
            if (islands) d = std::min(d, islands->capOf[v]);
            if (!std::isfinite(d)) continue;
            if (each) out.each.emplace_back(v, d);
            if (d > out.worst) { out.worst = d; out.vertex = v; }
        }
    });
    if (each) { each->clear(); each->reserve(removed.size()); }
    if (worstVertexOut) *worstVertexOut = std::numeric_limits<unsigned>::max();
    float worst = 0.0f;
    for (const Chunk &chunk : chunks) {
        if (each) each->insert(each->end(), chunk.each.begin(), chunk.each.end());
        if (chunk.worst > worst) {
            worst = chunk.worst;
            if (worstVertexOut) *worstVertexOut = chunk.vertex;
        }
    }
    return worst;
}

/// THE PER-FACET TERM (ATOM-LOD-BOUND-1 F1): the distance from a union of
/// triangles is not convex, so a level-0 facet the level no longer has can peak
/// in its INTERIOR (a sliver between a removed tip near a wall and its kept ring)
/// where neither the removed-vertex walk nor a few thousand area samples look.
/// So every facet of `from` that is NOT a facet of `against` (a facet the level
/// kept is at distance zero everywhere) is queried at fixed points against
/// `againstGrid`: `Bake` = its centroid and three edge midpoints; `Reference` =
/// also its three corners and the centroids of its three corner sub-triangles
/// (the 1-to-4 split) — strictly denser, the verification's pattern. Level-0
/// points are island-capped (`islands`, null for the level's own facets). Only
/// the MAXIMUM is wanted, so each query stops at the running maximum
/// (`TriangleGrid::closest`'s `stopAtOrBelow`), which is what keeps it in the
/// vertex walk's cost class.
enum class FacetPattern { Bake, Reference };
float facetDistance(const float *positions, int posComps, const std::vector<unsigned> &from,
                    const std::vector<unsigned> &against, const surface::TriangleGrid &againstGrid,
                    const Islands *islands, FacetPattern pattern, surface::Queries &queries,
                    float worst = 0.0f, std::vector<unsigned> *overOut = nullptr)
{
    // `overOut` (the displacement lock's form): collect every facet of `from`
    // with a point further than `worst` instead of raising the maximum.
    const float threshold = worst;
    if (overOut) overOut->clear();
    std::vector<std::array<unsigned, 3>> present(against.size() / 3);
    for (size_t t = 0; t < present.size(); ++t) {
        std::array<unsigned, 3> f = { against[t * 3], against[t * 3 + 1], against[t * 3 + 2] };
        std::sort(f.begin(), f.end());
        present[t] = f;
    }
    std::sort(present.begin(), present.end());
    const auto at = [&](unsigned v) {
        const float *p = positions + size_t(v) * size_t(posComps);
        return Vec3(p[0], p[1], p[2]);
    };
    // THE FACETS ON THE BAKE'S THREADS. Each chunk keeps its OWN running maximum,
    // starting at the caller's `worst`, as the stop of its queries. That changes
    // how much each query walks, never the answer: a query stopped at the running
    // maximum returns a distance at or below it only when the true one is too (and
    // then it cannot raise the maximum), and otherwise walks to the exact nearest —
    // so every chunk ends at max(worst, the exact maximum of its points), and the
    // fold over chunks is the serial loop's number. The `overOut` list is per chunk
    // against the fixed `threshold`, concatenated in chunk (= facet) order.
    struct Chunk { float worst = 0.0f; std::vector<unsigned> over; };
    const bakepool::Range range(from.size() / 3, kQueryGrain);
    std::vector<Chunk> chunks(range.count);
    bakepool::forRange(range, int(queries.size()), [&](size_t chunk, size_t begin, size_t end, int slot) {
        surface::TriangleGrid::Query &query = queries[size_t(slot)];
        float local = worst;
        std::vector<unsigned> &over = chunks[chunk].over;
        for (size_t t = begin; t < end; ++t) {
            std::array<unsigned, 3> f = { from[t * 3], from[t * 3 + 1], from[t * 3 + 2] };
            std::sort(f.begin(), f.end());
            if (std::binary_search(present.begin(), present.end(), f)) continue;
            const float cap = islands ? islands->capOf[from[t * 3]]
                                      : std::numeric_limits<float>::infinity();
            if (!(cap > (overOut ? threshold : local))) continue;   // this island cannot exceed it
            const Vec3 a = at(from[t * 3]), b = at(from[t * 3 + 1]), c = at(from[t * 3 + 2]);
            Vec3 pts[10];
            int n = 0;
            pts[n++] = (a + b + c) * (1.0f / 3.0f);
            pts[n++] = (a + b) * 0.5f;
            pts[n++] = (b + c) * 0.5f;
            pts[n++] = (c + a) * 0.5f;
            if (pattern == FacetPattern::Reference) {
                pts[n++] = a; pts[n++] = b; pts[n++] = c;
                pts[n++] = (a * 4.0f + b + c) * (1.0f / 6.0f);
                pts[n++] = (b * 4.0f + c + a) * (1.0f / 6.0f);
                pts[n++] = (c * 4.0f + a + b) * (1.0f / 6.0f);
            }
            for (int i = 0; i < n; ++i) {
                const float stop = overOut ? threshold : local;
                const float d = std::min(againstGrid.closest(query, pts[i], nullptr, nullptr, nullptr, stop), cap);
                if (!std::isfinite(d) || d <= stop) continue;
                if (overOut) { over.push_back(unsigned(t)); break; }
                local = d;
            }
        }
        chunks[chunk].worst = local;
    });
    for (const Chunk &chunk : chunks) {
        worst = std::max(worst, chunk.worst);
        if (overOut) overOut->insert(overOut->end(), chunk.over.begin(), chunk.over.end());
    }
    return worst;
}

/// Both terms at once (the verification's form): returns the exact term and
/// writes the sampled one, before any margin, to `areaOut`.
float twoSidedDistance(const float *positions, int posComps,
                       const std::vector<unsigned> &a, const surface::TriangleGrid &gridA,
                       const std::vector<unsigned> &b, const surface::TriangleGrid &gridB,
                       size_t samples, float *areaOut, const Islands *islands,
                       surface::Queries &queries)
{
    if (areaOut)
        *areaOut = areaDistance(positions, posComps, a, gridA, b, gridB, samples, islands, queries);
    return removedVertexDistance(positions, posComps, vertexSet(b.data(), b.size()), a, gridA,
                                 islands, queries);
}

/// The first vertex buffer carrying `usage`, as floats: pointer, component
/// count and how many vertices it holds. Null when the mesh has no such buffer.
const float *attribData(const MeshPtr &mesh, VertexAttribUsage usage,
                        int *componentsOut, size_t *countOut)
{
    for (const VertexBufferPtr &vb : mesh->getVertexBuffers()) {
        if (!vb || !vb->data || vb->dataSize <= 0) continue;
        const QList<VertexAttribute> attribs = vb->vertexLayout.getAttribs();
        if (attribs.isEmpty()) continue;
        const VertexAttribute &a = attribs.first();
        if (a.usage != usage) continue;
        const int comps = a.count > 0 ? a.count : 3;
        if (componentsOut) *componentsOut = comps;
        if (countOut) *countOut = size_t(vb->dataSize) / (sizeof(float) * size_t(comps));
        return reinterpret_cast<const float *>(vb->data);
    }
    return nullptr;
}

void build(const MeshPtr &mesh, QVector<MeshBake::LodLevelTerms> *terms)
{
    if (terms) terms->clear();
    if (mesh.isNull()) return;
    mesh->lodIndices.clear();
    mesh->lodErrors.clear();
    mesh->lodBounds.clear();
    if (!mesh->getSkeleton().isNull()) return;           // static meshes only, stage 1
    if (mesh->primitiveMode != PrimitiveMode::Triangles) return;

    int posComps = 3; size_t nv = 0;
    const float *positions = attribData(mesh, VertexAttribUsage::Position, &posComps, &nv);
    if (!positions || posComps < 3 || nv < 3) return;
    const IndexBufferPtr ib = mesh->getIndexBuffer();
    if (ib.isNull() || !ib->data || ib->dataSize <= 0) return;

    std::vector<unsigned> base(reinterpret_cast<const unsigned *>(ib->data),
                               reinterpret_cast<const unsigned *>(ib->data) +
                                   size_t(ib->dataSize) / sizeof(unsigned));
    if (base.size() < 3 || base.size() % 3 != 0) return;
    for (unsigned i : base) if (size_t(i) >= nv) return;   // a malformed index list simplifies to nothing good
    if (base.size() / 3 < size_t(kMinTriangles) * 2) return;

    int nrmComps = 3; size_t nrmCount = 0;
    const float *normals = attribData(mesh, VertexAttribUsage::Normal, &nrmComps, &nrmCount);
    if (normals && nrmCount < nv) normals = nullptr;   // a short normal buffer is not a metric
    int uvComps = 2; size_t uvCount = 0;
    const float *uvs = attribData(mesh, VertexAttribUsage::TexCoord0, &uvComps, &uvCount);
    if (uvs && (uvCount < nv || uvComps < 2)) uvs = nullptr;
    const size_t posStride = sizeof(float) * size_t(posComps);

    // The extent the relative-error cap is measured against, and the length the
    // UV weight is derived from — meshoptimizer's own scaling factor, i.e. the
    // mesh's largest axis extent.
    const float extent = meshopt_simplifyScale(positions, nv, posStride);

    // The attribute buffer, interleaved once: [nx ny nz] [u v], whichever of
    // the two the mesh has.
    std::vector<float> attribs;
    std::vector<float> weights;
    const size_t attrCount = (normals ? 3u : 0u) + (uvs ? 2u : 0u);
    if (attrCount) {
        attribs.assign(nv * attrCount, 0.0f);
        weights.assign(attrCount, kNormalWeight);
        for (size_t v = 0; v < nv; ++v) {
            float *dst = &attribs[v * attrCount];
            if (normals) { for (int c = 0; c < 3; ++c) dst[c] = normals[v * size_t(nrmComps) + size_t(c)]; dst += 3; }
            if (uvs)     { for (int c = 0; c < 2; ++c) dst[c] = uvs[v * size_t(uvComps) + size_t(c)]; }
        }
        if (uvs) {
            float umin = FLT_MAX, umax = -FLT_MAX, vmin = FLT_MAX, vmax = -FLT_MAX;
            for (size_t v = 0; v < nv; ++v) {
                const float u = uvs[v * size_t(uvComps)], w = uvs[v * size_t(uvComps) + 1];
                umin = std::min(umin, u); umax = std::max(umax, u);
                vmin = std::min(vmin, w); vmax = std::max(vmax, w);
            }
            float range = std::max(umax - umin, vmax - vmin);
            if (!(range > 1e-6f)) range = 1.0f;
            const float uvWeight = kUvWeight * (extent > 0.0f ? extent : 1.0f) / range;
            weights[attrCount - 2] = uvWeight;
            weights[attrCount - 1] = uvWeight;
        }
    }

    // THE NEAREST-SURFACE GRID OVER LEVEL 0, built ONCE for the whole chain —
    // every level is measured against the AUTHORED geometry and never against
    // its predecessor, because what a consumer of level k needs to know is how
    // far k may sit from what the artist made, not from level k-1. (Composing
    // consecutive distances would be an upper bound of an upper bound: correct
    // and needlessly loose, and it is not what any consumer asks.)
    Vec3 lo(positions[0], positions[1], positions[2]), hi = lo;
    for (size_t v = 0; v < nv; ++v) {
        const float *p = positions + v * size_t(posComps);
        lo = Vec3(std::min(lo.x(), p[0]), std::min(lo.y(), p[1]), std::min(lo.z(), p[2]));
        hi = Vec3(std::max(hi.x(), p[0]), std::max(hi.y(), p[1]), std::max(hi.z(), p[2]));
    }
    surface::TriangleGrid baseGrid;
    baseGrid.build(positions, posComps, base, lo, hi, boundGridRes(base.size() / 3));
    const size_t boundSamples = base.size() / 3 > size_t(kBoundBigTriangles)
                                    ? size_t(kBoundSamplesBig) : size_t(kBoundSamples);

    // THE ISLANDS (the represented-surface rule, `Islands` above): once, over
    // level 0 — every level is measured against level 0 and every lock names a
    // level-0 component.
    const Islands islands = findIslands(positions, posComps, nv, base);
    const std::vector<unsigned> baseSet = vertexSet(base.data(), base.size());
    surface::Queries queries(static_cast<size_t>(bakepool::width()));   // one query scratch per thread
    std::vector<unsigned char> lock;            // per vertex; allocated on first need
    std::vector<std::pair<unsigned, float>> displaced;
    std::vector<size_t> survivors(islands.count());

    std::vector<unsigned> prev = base;
    float accumulated = 0.0f;
    float boundSoFar = 0.0f;
    for (int level = 0; level < kMaxLevels; ++level) {
        size_t target = size_t(float(prev.size()) * kRatio);
        target -= target % 3;
        if (target / 3 < size_t(kMinTriangles)) break;

        std::vector<unsigned> out(prev.size());
        float stepError = 0.0f;
        const unsigned options = meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPermissive;
        const auto simplify = [&](const unsigned char *vertexLock) {
            return attrCount
                ? meshopt_simplifyWithAttributes(out.data(), prev.data(), prev.size(),
                                                 positions, nv, posStride,
                                                 attribs.data(), sizeof(float) * attrCount,
                                                 weights.data(), attrCount,
                                                 vertexLock, target, FLT_MAX, options, &stepError)
                : meshopt_simplifyWithAttributes(out.data(), prev.data(), prev.size(),
                                                 positions, nv, posStride,
                                                 nullptr, 0, nullptr, 0,
                                                 vertexLock, target, FLT_MAX, options, &stepError);
        };
        size_t n = simplify(nullptr);
        // THE DISPLACEMENT LOCK (the represented-surface rule's other half): after a
        // simplification every removed level-0 vertex is measured against the
        // level's surface (island-capped); one displaced by more than
        // `kDisplacementBudget` times the level's own error is LOCKED and the level
        // simplified again. A deleted island bigger than the budget is thereby
        // kept (all its vertices are displaced by its extent), and a spike or a
        // sliver the quadric under-priced survives to a coarser level. The first
        // pass is the plain one, so a mesh whose simplification already honours
        // the budget (every shipped primitive, the dragon) takes exactly the path
        // it always took. The last pass's measurement IS the level's vertex term.
        int passes = 1, locked = 0;
        bool locking = false;
        std::vector<size_t> anchors;             // per component: its largest triangle in prev
        std::vector<unsigned> fanStart, fanTris; // CSR: each vertex's triangles in prev
        std::vector<unsigned> overFacets;        // level-0 facets past the budget (F1)
        std::vector<unsigned> candidate;
        surface::TriangleGrid levelGrid;
        float vertexTerm = 0.0f;
        unsigned worstVertex = std::numeric_limits<unsigned>::max();
        while (n >= 3 && n % 3 == 0) {
            candidate.assign(out.begin(), out.begin() + std::ptrdiff_t(n));
            levelGrid.build(positions, posComps, candidate, lo, hi, boundGridRes(candidate.size() / 3));
            vertexTerm = removedVertexDistance(positions, posComps, baseSet, candidate, levelGrid,
                                               &islands, queries, &displaced, &worstVertex);
            if (passes >= kLockPasses) break;
            const float budget = kDisplacementBudget * std::max(accumulated, stepError);
            bool more = false;
            // A displaced vertex is kept WITH ITS FAN: the vertex and every vertex of
            // its triangles in `prev`. Locking the vertex alone is not enough — a
            // locked vertex still leaves the index list when the collapses of its
            // NEIGHBOURS degenerate every triangle it is in, and a fully locked
            // triangle cannot degenerate.
            if (fanStart.empty()) {
                fanStart.assign(nv + 1, 0);
                for (unsigned v : prev) ++fanStart[size_t(v) + 1];
                for (size_t v = 0; v < nv; ++v) fanStart[v + 1] += fanStart[v];
                fanTris.resize(prev.size());
                std::vector<unsigned> fill(fanStart.begin(), fanStart.end() - 1);
                for (size_t i = 0; i < prev.size(); ++i) fanTris[fill[prev[i]]++] = unsigned(i / 3);
            }
            const auto lockVertex = [&](unsigned v) {
                if (!locking) { lock.assign(nv, 0); locking = true; }
                if (lock[v]) return false;
                lock[v] = meshopt_SimplifyVertex_Lock;
                ++locked;
                return true;
            };
            const auto lockWithFan = [&](unsigned v) {
                if (fanStart[v] == fanStart[size_t(v) + 1]) return;   // not in prev: not this level's to keep
                if (lockVertex(v)) more = true;
                for (unsigned f = fanStart[v]; f < fanStart[size_t(v) + 1]; ++f)
                    for (int k = 0; k < 3; ++k)
                        if (lockVertex(prev[size_t(fanTris[f]) * 3 + size_t(k)])) more = true;
            };
            for (const auto &vd : displaced)
                if (vd.second > budget) lockWithFan(vd.first);
            // ...AND EVERY DROPPED LEVEL-0 FACET WITH AN INTERIOR POINT PAST THE
            // BUDGET (F1: the distance peaks mid-facet on slivers the vertex walk
            // cannot see) keeps its corners, with their fans.
            facetDistance(positions, posComps, base, candidate, levelGrid, &islands,
                          FacetPattern::Bake, queries, budget, &overFacets);
            for (unsigned t : overFacets)
                for (int k = 0; k < 3; ++k) lockWithFan(base[size_t(t) * 3 + size_t(k)]);
            if (!more) break;
            // AND EVERY COMPONENT WHOSE ERASURE WOULD COST MORE THAN THE BUDGET
            // KEEPS ONE TRIANGLE — its largest in `prev`, all three vertices
            // locked (a triangle whose vertices cannot move cannot degenerate).
            // Without it the re-simplification pays for the vertices just locked
            // by erasing OTHER islands the first pass happened to leave, and the
            // loop chases them pass after pass (measured on the scan: 700-2700
            // locks and the pass budget exhausted on two of three meshes).
            if (anchors.empty()) {
                anchors.assign(islands.count(), std::numeric_limits<size_t>::max());
                std::vector<float> best(islands.count(), -1.0f);
                for (size_t t = 0; t < prev.size() / 3; ++t) {
                    const float *a = positions + size_t(prev[t * 3]) * size_t(posComps);
                    const float *b = positions + size_t(prev[t * 3 + 1]) * size_t(posComps);
                    const float *c = positions + size_t(prev[t * 3 + 2]) * size_t(posComps);
                    const float area = Vec3::crossProduct(Vec3(b[0] - a[0], b[1] - a[1], b[2] - a[2]),
                                                          Vec3(c[0] - a[0], c[1] - a[1], c[2] - a[2]))
                                           .lengthSquared();
                    const unsigned comp = islands.compOf[prev[t * 3]];
                    if (area > best[comp]) { best[comp] = area; anchors[comp] = t; }
                }
            }
            for (size_t comp = 0; comp < islands.count(); ++comp) {
                if (anchors[comp] == std::numeric_limits<size_t>::max() ||
                    islands.extent[comp] <= budget) continue;
                for (int k = 0; k < 3; ++k) {
                    const unsigned v = prev[anchors[comp] * 3 + size_t(k)];
                    lockVertex(v);
                }
            }
            n = simplify(lock.data());
            ++passes;
        }
        if (n < 3 || n % 3 != 0) break;
        // Topology can stop the simplifier short. A level that did not shed at
        // least 15% costs a buffer and a switch for nothing.
        if (float(n) > float(prev.size()) * kAcceptRatio) break;

        const float error = std::max(accumulated, stepError);
        if (extent > 0.0f && error > extent * kMaxRelError) break;

        out.swap(candidate);

        // THE MEASUREMENT (AT-A5). Level `out` against level 0, both ways, every
        // level-0 point capped at its island's extent (the represented-surface
        // rule); the sampling-gap margin on the sampled term only.
        const float areaTerm = areaDistance(positions, posComps, out, levelGrid, base, baseGrid,
                                            boundSamples, &islands, queries);
        // THE EXACT TERM IS VERTICES AND FACETS (F1): the removed-vertex walk
        // above, then every level-0 facet the level dropped (centroid + edge
        // midpoints, island-capped) against the level, and every facet the level
        // made against level 0 (surface it ADDED). Exact per point, no margin.
        float facetTerm = facetDistance(positions, posComps, base, out, levelGrid, &islands,
                                        FacetPattern::Bake, queries, vertexTerm);
        facetTerm = facetDistance(positions, posComps, out, base, baseGrid, nullptr,
                                  FacetPattern::Bake, queries, facetTerm);
        const float exactTerm = std::max(vertexTerm, facetTerm);
        const float measured = std::max(areaTerm * kBoundMargin, exactTerm);
        // MONOTONE NON-DECREASING BY CONSTRUCTION, and that is a requirement and
        // not a tidy-up: `lodLevelForWorldError` walks the array and stops at the
        // FIRST level it cannot afford, which is only the right answer for a
        // sorted array. A sampled maximum is not guaranteed to rise with the
        // level (a coarser level can happen to land closer at the points these
        // samples fall on), so the running max is taken — which over-states, i.e.
        // errs towards a FINER level than needed, which is the safe direction.
        const float bound = std::max(std::max(boundSoFar, measured),
                                     extent > 0.0f ? extent * kBoundFloorRel : 0.0f);
        // The safety net (`kMaxRelBound`): this level's surface may sit further
        // from the object than a quarter of the object. Not a stand-in — stop.
        if (extent > 0.0f && bound > extent * kMaxRelBound) break;

        QVector<quint32> levelIndices;
        levelIndices.resize(int(n));
        std::memcpy(levelIndices.data(), out.data(), n * sizeof(unsigned));
        mesh->lodIndices.append(levelIndices);
        mesh->lodErrors.append(error);
        mesh->lodBounds.append(bound);
        if (terms) {
            MeshBake::LodLevelTerms row;
            row.triangles = int(n / 3);
            row.quadric = error;
            row.areaTerm = areaTerm * kBoundMargin;
            row.vertexTerm = vertexTerm;
            row.facetTerm = facetTerm;
            row.bound = bound;
            row.verticesLocked = locked;
            row.passes = passes;
            std::fill(survivors.begin(), survivors.end(), size_t(0));
            for (size_t t = 0; t < out.size() / 3; ++t) ++survivors[islands.compOf[out[t * 3]]];
            for (size_t c = 0; c < islands.count(); ++c)
                if (!survivors[c]) {
                    ++row.islandsDropped;
                    row.droppedMaxExtent = std::max(row.droppedMaxExtent, islands.extent[c]);
                }
            if (worstVertex < nv && islands.compOf[worstVertex] < islands.count()) {
                row.worstComponentTriangles = int(islands.triangles[islands.compOf[worstVertex]]);
                row.worstComponentExtent = islands.extent[islands.compOf[worstVertex]];
            }
            terms->append(row);
        }

        accumulated = error;
        boundSoFar = bound;
        prev.swap(out);
    }
}

}   // namespace lodchain

// ---------------------------------------------------------------------------
// SURFACE-CACHE phase 1 — THE CARD GENERATOR (SPECS/SURFACE_CACHE_ASSESSMENT.md
// §2, §4, §7 phase 1; the phase-0 measurement in
// ~/Developer/spikes/surface-cache-0/FINDINGS.md).
//
// A CARD is an axis-aligned orthographic capture of a patch of this mesh's
// surface (document/assets/mesh.h MeshCard). Phase 2 captures each card into an
// atlas; phase 4 lights a ray hit from the card under it instead of from the
// cascade's voxel. Neither exists yet — what this builds is the LIST, and the
// list is the BUDGET those phases spend: SURFACE-CACHE-0 measured the capture's
// cost as FIXED PER CARD (0.042-0.057 ms of CPU, 9-11 us of GPU, 352 KB at
// 128^2), so the number and the shape of the cards decided here IS the cost of
// the cache.
//
// THE SHAPE IS LUMEN'S, and every step of it is here for a reason that can be
// stated:
//
//   1. SURFELS, area-weighted. The thing a card must cover is SURFACE, not
//      vertices and not triangles: a mesh with one enormous floor triangle and
//      ten thousand tiny ones in a corner would be clustered entirely in the
//      corner by any per-triangle scheme. The sampling is STRATIFIED over the
//      cumulative-area array and the barycentric coordinates come from a van
//      der Corput pair, so there is no random number anywhere in this file and
//      "deterministic with a fixed seed" is stronger than asked: there is no
//      seed. A surfel carries the GEOMETRIC normal of its triangle, because the
//      capture rasters geometry and a shading normal can point somewhere the
//      surface does not face.
//
//   2. CLUSTERING — K-means on position + normal, with the normal term taken to
//      the only value a CARD can honour. Lumen clusters surfels on position and
//      normal and turns each cluster into ONE axis-aligned direction. A card
//      HAS one axis, so two surfels whose dominant axes differ can never share
//      a card usefully however close they are: the normal term's weight, for
//      axis-aligned cards, is effectively infinite. So the surfels are first
//      partitioned by their dominant axis (the six-way assignment IS the normal
//      term) and K-means then runs on POSITION inside each bucket. That is the
//      same algorithm with the weight that the card's own geometry forces, and
//      it cannot produce the failure a finite weight can — a cluster whose mean
//      normal points between two axes, whose card then faces neither half of it.
//
//   3. ONE CARD PER CLUSTER: the cluster's bounding rectangle in the card
//      plane, its depth range along the axis, both grown by a margin (the
//      surfels are samples of a surface, not its corners), and the LOD LEVEL
//      whose error is below the card's own texel — ATOM-2's rule
//      (`lodLevelForWorldError`, jahshaka/engine/Types.h) with the card texel
//      in the cell's place.
//
//   4. THE 6-FACE BOX FALLBACK, and it is a MEASUREMENT, not a guess. Lumen
//      falls back to the box for "meshes that yield few clusters". Rather than
//      guess what "few" is, the generator measures the clustered list's
//      coverage against the real surface; if it is below `kGoodCoverage` the
//      box is built too, measured the same way, and the better list wins. On a
//      cube the two lists ARE the same six cards, which is why the suite can
//      assert the box exactly there.
//
//   5. COVERAGE IS MEASURED WITH OCCLUSION. A surfel is covered by a card when
//      it FACES the card, lies inside its rectangle and depth range, AND is the
//      nearest surface of the mesh along the card's ray — which is what the
//      capture will actually record. Without the last term coverage is 1.0 by
//      construction (every surfel is inside the card built around it) and the
//      >= 0.9 gate measures nothing. The occlusion term is a small CPU depth
//      raster of the mesh into the card's frame at `kCoverageResolution`.
//
// WHAT GETS NO CARDS: a skinned mesh (its surface moves, so a card baked
// against the bind pose is a lie — Epic's own limit, and the reason a skinned
// hit keeps reading the voxel fallback), a mesh that is not triangles, and a
// mesh with no usable area. Empty is an honest answer and costs four bytes.
namespace cards {

/// THE KNOBS, in one place, with the reason each exists. These are BAKE INPUTS
/// — meshbake.cpp is hashed into the producer id, so editing any of them
/// re-bakes every library by itself.
constexpr int   kCaptureResolution  = 128;    ///< Lumen's page size: the texel a card's LOD level is chosen for. Phase 2 splits cards larger than this; the LEVEL is what the bake owes.
constexpr int   kCoverageResolution = 64;     ///< the depth raster coverage is measured on. Half the capture's: coverage is a quality number, not the picture.
constexpr int   kMinSurfels         = 256;    ///< a 12-triangle cube still needs enough samples to show six faces.
constexpr int   kMaxSurfels         = 4096;   ///< and K-means is O(surfels x cards x iterations): this is the ceiling the bake time is bounded by.
constexpr int   kSurfelsPerTriangle = 2;      ///< between the two, a mesh gets this many per triangle.
constexpr int   kKMeansIterations   = 12;     ///< Lloyd converges long before this on 3-D position clusters; it is a bound, not a target.
constexpr float kFacingMin          = 0.10f;  ///< below this a surface is too edge-on for its card to record it usefully (84 degrees).
constexpr float kMarginFraction     = 0.02f;  ///< a card is grown by this fraction of the mesh extent: surfels are samples of a surface, not its corners.
constexpr float kDepthTolerance     = 1.5f;   ///< coverage depth test, in card texels, divided by how squarely the surfel faces the card (a slope moves more depth per texel).
constexpr float kMinSplitGain       = 0.001f; ///< a split must cover at least one more surfel in a thousand — it must DO something. It is deliberately tiny: the thing that stops the budget being spent is kGoodCoverage below, not this. A bigger threshold looked reasonable and was measurably wrong: on a self-occluding mesh one extra card gains a fraction of a percent and the NEXT one gains two, so a per-split toll of half a percent stopped the star at 0.797 where the budget could reach 0.85.
constexpr float kClusterDepthWeight = 4.0f;   ///< K-means runs in the CARD'S OWN frame with DEPTH weighted this much above the two in-plane axes. Measured, not guessed: a hemisphere's +Y bucket holds the dome AND the up-facing floor cap under it, and on unweighted position the two farthest points are opposite RIM points — the split separates the cap from itself and buys nothing (coverage stuck at 0.667, 512 of 835 surfels in that one bucket unseen). Two patches at the same (u,v) and DIFFERENT depth are the ones one card cannot both capture; two at different (u,v) and the same depth are captured fine by one. Depth is what a split is FOR.
constexpr float kGoodCoverage       = 0.90f;  ///< below this the 6-face box is built and measured too, and the better list wins.
constexpr int   kCoverageMaxTriangles = 50000; ///< above this the coverage raster uses the COARSEST baked level instead of the authored geometry, with the level's own error added to the tolerance — the bake must not grow with the model.

/// A SURFEL is a surface::Sample plus the one card axis that can record it.
/// (The sampler itself moved out of this file's card half in ATOM-BAKE-1 — see
/// `namespace surface` above: the LOD chain's honest error samples the same
/// surface the same way, and two samplers would be two definitions of "a point
/// of this mesh".)
struct Surfel
{
    Vec3 pos;
    Vec3 nrm;
    int axis = 0;
};

/// The dominant one of the six axis directions for a normal, and its dot.
int dominantAxis(const Vec3 &n, float *dotOut)
{
    const float c[3] = { n.x(), n.y(), n.z() };
    int best = 0; float bestDot = -2.0f;
    for (int a = 0; a < 3; ++a) {
        if (c[a] > bestDot)  { bestDot = c[a];  best = a * 2; }
        if (-c[a] > bestDot) { bestDot = -c[a]; best = a * 2 + 1; }
    }
    if (dotOut) *dotOut = bestDot;
    return best;
}

/// THE ONE LOD RULE (jahshaka/engine/Types.h `lodLevelForWorldError`), restated
/// here because the bake is document-side and links no engine: the coarsest
/// level whose BOUND is STRICTLY below what the consumer can afford. The card's
/// consumer affords its own texel.
///
/// `lodBounds` and NOT `lodErrors` since ATOM-BAKE-1 (AT-A5): the simplifier's
/// number is an estimate and a card that trusted it captured a level whose real
/// surface sits further from the authored one than the texel it was chosen for.
/// AND THIS IS THE CARD'S ONLY PRODUCER (AT-CARDLOD): the capture used to
/// re-derive the level from the card's real atlas texel and keep the baked value
/// as a fallback, so one number had two producers that could disagree. The bake
/// owns it.
int levelForTexel(const MeshPtr &mesh, float texel)
{
    if (!(texel > 0.0f)) return 0;
    int level = 0;
    for (int i = 0; i < mesh->lodBounds.size() && i < mesh->lodIndices.size(); ++i) {
        if (!(mesh->lodBounds.at(i) < texel)) break;   // bounds are non-decreasing
        level = i + 1;
    }
    return level;
}

/// The mesh's AABB projected onto a card axis's plane — the limit a card's
/// rectangle is clamped to.
void planeLimits(int axis, const Vec3 &lo, const Vec3 &hi,
                 float *uMin, float *uMax, float *vMin, float *vMax,
                 float *dMin = nullptr, float *dMax = nullptr)
{
    const Vec3 U = MeshCard::axisU(axis);
    const Vec3 V = MeshCard::axisV(axis);
    const Vec3 D = MeshCard::axisDirection(axis);
    *uMin = FLT_MAX; *uMax = -FLT_MAX; *vMin = FLT_MAX; *vMax = -FLT_MAX;
    float dLo = FLT_MAX, dHi = -FLT_MAX;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 p((corner & 1) ? hi.x() : lo.x(),
                     (corner & 2) ? hi.y() : lo.y(),
                     (corner & 4) ? hi.z() : lo.z());
        const float u = Vec3::dotProduct(p, U);
        const float v = Vec3::dotProduct(p, V);
        const float d = Vec3::dotProduct(p, D);
        *uMin = std::min(*uMin, u); *uMax = std::max(*uMax, u);
        *vMin = std::min(*vMin, v); *vMax = std::max(*vMax, v);
        dLo = std::min(dLo, d); dHi = std::max(dHi, d);
    }
    if (dMin) *dMin = dLo;
    if (dMax) *dMax = dHi;
}

/// A card built around a set of points, in the card's own frame.
///
/// The rectangle is grown by `margin` (surfels are SAMPLES of a surface, not
/// its corners, so the bounding box of the samples always falls short of it)
/// and then CLAMPED TO THE MESH'S OWN BOX in the card's plane: a card wider
/// than the object it captures spends texels on empty space, and the clamp is
/// what makes a convex mesh's card list the exact 6-face box rather than a box
/// plus a margin. The DEPTH margin is deliberately NOT clamped — it is the
/// capture's standoff, and a near plane sitting exactly on the surface is a
/// z-fight, not a tight fit.
MeshCard fromBounds(int axis, float uMin, float uMax, float vMin, float vMax,
                    float dMin, float dMax, float margin,
                    const Vec3 &lo, const Vec3 &hi)
{
    float uLo, uHi, vLo, vHi;
    planeLimits(axis, lo, hi, &uLo, &uHi, &vLo, &vHi);
    uMin = std::max(uMin - margin, uLo); uMax = std::min(uMax + margin, uHi);
    vMin = std::max(vMin - margin, vLo); vMax = std::min(vMax + margin, vHi);

    MeshCard card;
    card.axis = quint8(axis);
    const Vec3 U = MeshCard::axisU(axis);
    const Vec3 V = MeshCard::axisV(axis);
    const Vec3 D = MeshCard::axisDirection(axis);
    card.origin = U * ((uMin + uMax) * 0.5f) + V * ((vMin + vMax) * 0.5f)
                  + D * ((dMin + dMax) * 0.5f);
    // A degenerate rectangle (a mesh with no extent in one axis) still has to
    // be a positive size: the reader refuses a card that is not.
    card.halfU = std::max((uMax - uMin) * 0.5f, margin);
    card.halfV = std::max((vMax - vMin) * 0.5f, margin);
    card.halfDepth = (dMax - dMin) * 0.5f + margin;
    return card;
}

/// The depth raster + the surfel test, for ONE card: fills `seen` (one bit per
/// surfel) and returns this card's own coverage fraction.
float measure(const MeshCard &card, const std::vector<Surfel> &surfels,
              const float *positions, int posComps, const std::vector<unsigned> &indices,
              float extraTolerance, std::vector<char> *seen)
{
    const Vec3 U = MeshCard::axisU(card.axis);
    const Vec3 V = MeshCard::axisV(card.axis);
    const Vec3 D = MeshCard::axisDirection(card.axis);
    const float width = card.halfU * 2.0f, height = card.halfV * 2.0f;
    const float depth = card.halfDepth * 2.0f;
    if (!(width > 0.0f) || !(height > 0.0f) || !(depth > 0.0f)) return 0.0f;

    const int R = kCoverageResolution;
    // Depth from the card's NEAR plane, growing away from the camera; FLT_MAX =
    // nothing there.
    std::vector<float> zbuf(size_t(R) * size_t(R), FLT_MAX);
    const float uOrigin = Vec3::dotProduct(card.origin, U) - card.halfU;
    const float vOrigin = Vec3::dotProduct(card.origin, V) - card.halfV;
    const float dNear   = Vec3::dotProduct(card.origin, D) + card.halfDepth;

    const auto project = [&](const Vec3 &p, float *su, float *sv, float *sd) {
        *su = (Vec3::dotProduct(p, U) - uOrigin) / width * float(R);
        *sv = (Vec3::dotProduct(p, V) - vOrigin) / height * float(R);
        *sd = dNear - Vec3::dotProduct(p, D);
    };

    // A plain scanline-free bounding-box raster with barycentric depth. Nothing
    // is culled: an orthographic capture records the nearest surface whatever
    // its facing, so a back face that occludes must occlude here too.
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        Vec3 p[3];
        float x[3], y[3], z[3];
        for (int k = 0; k < 3; ++k) {
            const float *v = positions + size_t(indices[t + size_t(k)]) * size_t(posComps);
            p[k] = Vec3(v[0], v[1], v[2]);
            project(p[k], &x[k], &y[k], &z[k]);
        }
        const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (std::fabs(area) < 1e-9f) continue;
        int x0 = int(std::floor(std::min(std::min(x[0], x[1]), x[2])));
        int x1 = int(std::ceil (std::max(std::max(x[0], x[1]), x[2])));
        int y0 = int(std::floor(std::min(std::min(y[0], y[1]), y[2])));
        int y1 = int(std::ceil (std::max(std::max(y[0], y[1]), y[2])));
        x0 = std::max(x0, 0); y0 = std::max(y0, 0);
        x1 = std::min(x1, R - 1); y1 = std::min(y1, R - 1);
        bool wrote = false;
        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                const float cx = float(px) + 0.5f, cy = float(py) + 0.5f;
                float w0 = ((x[1] - cx) * (y[2] - cy) - (x[2] - cx) * (y[1] - cy)) / area;
                float w1 = ((x[2] - cx) * (y[0] - cy) - (x[0] - cx) * (y[2] - cy)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                wrote = true;
                const float zz = w0 * z[0] + w1 * z[1] + w2 * z[2];
                // THE CARD'S OWN NEAR AND FAR PLANES CLIP, exactly as the
                // capture's orthographic frustum will. This is not a detail: it
                // is the whole mechanism by which a SECOND card on the same
                // axis, at a nearer depth range, can see surface the first one
                // cannot — which is how Lumen's clusters cover a concave mesh,
                // and without it splitting a bucket gains nothing and the
                // greedy loop below correctly refuses to spend on it.
                if (zz < 0.0f || zz > depth) continue;
                float &slot = zbuf[size_t(py) * size_t(R) + size_t(px)];
                if (zz < slot) slot = zz;
            }
        }
        // A TRIANGLE SMALLER THAN A TEXEL COVERS NO PIXEL CENTRE, AND MUST STILL
        // OCCUPY ITS TEXEL. This is a coverage raster, not a picture: a dense
        // mesh's triangles are routinely finer than 1/64 of the card, so a
        // centre-sampled raster leaves most texels written by the one triangle
        // in N that happened to contain a centre — and a texel nothing wrote is
        // read below as "no surface here", which marks the surfels standing on
        // it UNCOVERED. Measured on the grid fixture: that, and not the
        // subsampler, is what made coverage fall with triangle count (1.000 at
        // 28.8k triangles against 0.399 at 259k), because thinning a
        // centre-sampled raster empties texels in direct proportion. Splatting
        // the centroid of a triangle that wrote nothing makes the depth buffer
        // dense whatever the density is, and it costs one test per triangle.
        if (!wrote) {
            const float cx = (x[0] + x[1] + x[2]) / 3.0f;
            const float cy = (y[0] + y[1] + y[2]) / 3.0f;
            if (cx >= 0.0f && cy >= 0.0f && cx < float(R) && cy < float(R)) {
                const float zz = (z[0] + z[1] + z[2]) / 3.0f;
                if (zz >= 0.0f && zz <= depth) {
                    float &slot = zbuf[size_t(cy) * size_t(R) + size_t(cx)];
                    if (zz < slot) slot = zz;
                }
            }
        }
    }

    const float texel = std::max(width, height) / float(R);
    int covered = 0;
    for (size_t i = 0; i < surfels.size(); ++i) {
        const Surfel &s = surfels[i];
        const float facing = Vec3::dotProduct(s.nrm, D);
        if (facing <= kFacingMin) continue;
        float su, sv, sd;
        project(s.pos, &su, &sv, &sd);
        if (su < 0.0f || sv < 0.0f || su >= float(R) || sv >= float(R)) continue;
        if (sd < -1e-4f || sd > depth + 1e-4f) continue;
        const float front = zbuf[size_t(sv) * size_t(R) + size_t(su)];
        if (front == FLT_MAX) continue;
        // A surface leaning away from the card moves more depth across one
        // texel, so the tolerance is the texel divided by how squarely it faces.
        const float tol = kDepthTolerance * texel / facing + extraTolerance;
        if (sd - front > tol) continue;          // something nearer is in the way
        ++covered;
        if (seen) (*seen)[i] = 1;
    }
    return surfels.empty() ? 0.0f : float(covered) / float(surfels.size());
}

/// The union coverage of a whole list, filling each card's own `coverage`.
float measureList(QVector<MeshCard> *list, const std::vector<Surfel> &surfels,
                  const float *positions, int posComps, const std::vector<unsigned> &indices,
                  float extraTolerance, std::vector<char> *seenOut = nullptr)
{
    std::vector<char> seen(surfels.size(), 0);
    for (MeshCard &card : *list)
        card.coverage = measure(card, surfels, positions, posComps, indices, extraTolerance, &seen);
    if (seenOut) *seenOut = seen;
    if (surfels.empty()) return 0.0f;
    size_t n = 0;
    for (char c : seen) n += size_t(c != 0);
    return float(n) / float(surfels.size());
}

void build(const MeshPtr &mesh, int maxCards)
{
    if (mesh.isNull()) return;
    mesh->cards.clear();
    mesh->cardCoverage = 0.0f;
    if (maxCards <= 0) return;                            // "no cards" is a legal request
    maxCards = std::min(maxCards, kMaxCardsCeiling);
    if (!mesh->getSkeleton().isNull()) return;            // a card on a bind pose is a lie
    if (mesh->primitiveMode != PrimitiveMode::Triangles) return;

    int posComps = 3; size_t nv = 0;
    const float *positions = lodchain::attribData(mesh, VertexAttribUsage::Position, &posComps, &nv);
    if (!positions || posComps < 3 || nv < 3) return;
    const IndexBufferPtr ib = mesh->getIndexBuffer();
    if (ib.isNull() || !ib->data || ib->dataSize <= 0) return;
    std::vector<unsigned> indices(reinterpret_cast<const unsigned *>(ib->data),
                                  reinterpret_cast<const unsigned *>(ib->data) +
                                      size_t(ib->dataSize) / sizeof(unsigned));
    if (indices.size() < 3 || indices.size() % 3 != 0) return;
    for (unsigned i : indices) if (size_t(i) >= nv) return;

    const auto vertexAt = [&](unsigned i) {
        const float *v = positions + size_t(i) * size_t(posComps);
        return Vec3(v[0], v[1], v[2]);
    };

    // ---- 1. surfels ------------------------------------------------------
    // THE SHARED SAMPLER (`namespace surface`), not a second one: the same
    // stratified area walk, the same van der Corput barycentrics, the same
    // geometric normal, the same skip of a degenerate triangle. A surfel is that
    // sample plus the one card axis that can record it.
    const size_t triCount = indices.size() / 3;
    const size_t sampleCount = size_t(std::clamp<long long>(
        (long long)triCount * kSurfelsPerTriangle, kMinSurfels, kMaxSurfels));
    std::vector<surface::Sample> samples;
    const float totalArea =
        surface::sample(positions, posComps, indices, sampleCount, &samples);
    if (!(totalArea > 0.0f)) return;                       // no area: nothing to card
    std::vector<Surfel> surfels;
    surfels.reserve(samples.size());
    for (const surface::Sample &sm : samples) {
        Surfel s;
        s.pos = sm.pos;
        s.nrm = sm.nrm;
        float dot = 0.0f;
        s.axis = dominantAxis(sm.nrm, &dot);
        surfels.push_back(s);
    }
    if (surfels.size() < 8) return;

    // The mesh's own extent — the margin's unit and the card's minimum size.
    Vec3 lo = surfels.front().pos, hi = surfels.front().pos;
    for (unsigned i : indices) {
        const Vec3 p = vertexAt(i);
        lo = Vec3(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
        hi = Vec3(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
    }
    const Vec3 size = hi - lo;
    const float extent = std::max(std::max(size.x(), size.y()), size.z());
    if (!(extent > 0.0f)) return;
    // Half the mean spacing between surfels, so a card never ends exactly on the
    // last sample it happened to draw.
    const float spacing = std::sqrt(totalArea / float(surfels.size())) * 0.5f;
    const float margin = std::max(kMarginFraction * extent, spacing);

    // THE GEOMETRY THE COVERAGE RASTER RUNS OVER, AND ITS HARD CEILING.
    //
    // The raster is the expensive half of this function and it is charged per
    // TRIANGLE PER CARD PER ROUND: `measureList` walks every card, and the
    // greedy loop below calls it once per round. At twelve cards and a dozen
    // rounds an unbounded raster is 144 x the mesh's triangle count in setups —
    // seconds on the import worker and, through Preferences' bake-all, seconds ON
    // THE UI THREAD. So the ceiling below is a rule,
    // not an optimisation, and it is applied in TWO steps because the first one
    // does not always fire:
    //
    //   1. the COARSEST BAKED LEVEL, when the mesh has a chain — the geometry a
    //      far-field capture would use anyway, and its own simplifier error is
    //      added to the depth tolerance because the level's surface really does
    //      sit that far from the authored one;
    //   2. a UNIFORM STRIDE through whatever step 1 left, when that is STILL
    //      above the ceiling. Step 1 misses two whole classes and they are not
    //      rare: a mesh whose topology stopped the simplifier before it shed
    //      anything (`kAcceptRatio`, documented by ATOM-1) has no chain at all,
    //      and neither does a mesh built outside the bake and handed to this
    //      generator directly (a procedural mesh, a suite). Those took the full
    //      index list, per card, per round.
    //
    // WHY THE SUBSAMPLE IS A GOLDEN-RATIO SEQUENCE AND NOT A UNIFORM STRIDE —
    // MEASURED, because a uniform stride was written first and it was WRONG.
    // A mesh is periodic: a triangulated grid alternates two triangle
    // orientations per quad, a lathed primitive repeats per segment. A stride
    // is a periodic sampler, so the two periods beat — a stride of 2 on the
    // grid fixture kept the SAME triangle of every quad and half the surface
    // was never rastered at all. Coverage on a fixture that reads 1.000 whole
    // fell to 0.735 at 64.8k triangles, 0.694 at 135k and 0.399 at 259k. The
    // selection below keeps triangle t when the low 32 bits of t x 2654435761
    // fall under a threshold: that multiplier is 2^32/phi, so consecutive t
    // walk a golden-ratio Weyl sequence, which is equidistributed and cannot
    // align with ANY mesh period. Same fixture, same ceiling: 1.000 / 1.000 /
    // 1.000, and the cost still flat.
    //
    // WHAT IT COSTS ANYWAY, stated honestly rather than as "conservative": a
    // dropped triangle can be a surfel's OWN, which leaves that texel empty and
    // the surfel UNcovered (an under-count), and it can be an OCCLUDER, which
    // leaves a hidden surfel looking visible (an over-count). The reason the
    // number stays usable is density — the subsample only engages above
    // `kCoverageMaxTriangles` triangles projected into a 64 x 64 raster, i.e.
    // hundreds of triangles per texel, and keeping one in N of hundreds still
    // fills every texel the silhouette covers. The tolerance is NOT widened for
    // it: dropping a triangle moves no surface, unlike taking a LOD level.
    const std::vector<unsigned> *rasterIndices = &indices;
    std::vector<unsigned> coarse;
    float extraTolerance = 0.0f;
    if (triCount > size_t(kCoverageMaxTriangles)) {
        if (!mesh->lodIndices.isEmpty()) {
            const QVector<quint32> &level = mesh->lodIndices.last();
            coarse.assign(level.constBegin(), level.constEnd());
            extraTolerance = mesh->lodBounds.isEmpty() ? 0.0f : mesh->lodBounds.last();
        }
        const std::vector<unsigned> &source = coarse.empty() ? indices : coarse;
        const size_t sourceTris = source.size() / 3;
        if (sourceTris > size_t(kCoverageMaxTriangles)) {
            const quint32 threshold = quint32(double(kCoverageMaxTriangles)
                                              / double(sourceTris) * 4294967296.0);
            std::vector<unsigned> kept;
            kept.reserve(size_t(kCoverageMaxTriangles) * 3 + 3);
            for (size_t t = 0; t < sourceTris; ++t) {
                if (quint32(quint32(t) * 2654435761u) >= threshold) continue;
                kept.push_back(source[t * 3]);
                kept.push_back(source[t * 3 + 1]);
                kept.push_back(source[t * 3 + 2]);
            }
            if (kept.size() >= 3) coarse.swap(kept);
        }
        if (!coarse.empty()) rasterIndices = &coarse;
    }

    // ---- 2/3. the clustered list, and the budget spent only where it BUYS
    // something -------------------------------------------------------------
    //
    // THE RULE: one card per non-empty axis bucket, then a second card in a
    // bucket only when the list MEASURABLY misses surface without it.
    //
    // Epic's number is a CEILING ("Lumen only places 12 Cards on a mesh, but
    // you can increase that amount"), not a quota to fill, and spending it
    // blindly is the expensive mistake here: a card is a capture pass forever
    // after (0.05 ms of CPU and 352 KB at 128^2, SURFACE-CACHE-0), so twelve
    // cards on a CUBE — which is what a proportional split of the budget gives,
    // two per face — costs double for a coverage of 0.992 against 0.992.
    // Splitting a patch in its own plane buys coverage only where the patch
    // OCCLUDES ITSELF; where it does not, one card records the same heightfield
    // with the same texels. (Card RESOLUTION is not a reason to split either:
    // Lumen splits a card into 128-texel pages at CAPTURE time, which is phase
    // 2's decision and not a thing the bake can know the atlas's page size for.)
    //
    // So the loop below is greedy and MEASURED: it gives the bucket with the
    // most surfels that nothing currently sees one more card, keeps the result
    // only if the union coverage moved by more than `kMinSplitGain`, and stops
    // at the budget, at `kGoodCoverage`, or the first time a split stops paying.
    std::vector<std::vector<size_t>> bucket(MeshCard::kAxisCount);
    for (size_t i = 0; i < surfels.size(); ++i) bucket[size_t(surfels[i].axis)].push_back(i);

    // Build the whole card list for a given per-bucket card count. Pure
    // function of the quota, so the greedy loop can try one and throw it away.
    // `seenHint`, when given, is the CURRENT coverage of the surfels: the LAST
    // centre of a bucket that is gaining a card is then seeded at the centroid
    // of that bucket's UNSEEN surfels instead of by farthest-point. Farthest
    // point puts a new boundary where the bucket's SPREAD is, which is very
    // often not where the misses are — on the star it split arms that were
    // already covered, the candidate gained nothing, and the bucket was retired
    // with a third of its surface unseen at eight cards of a budget of twelve.
    const auto listFor = [&](const std::vector<int> &quota,
                             const std::vector<char> *seenHint = nullptr) {
        QVector<MeshCard> list;
        for (int a = 0; a < MeshCard::kAxisCount; ++a) {
            const std::vector<size_t> &members = bucket[size_t(a)];
            const int k = std::min(quota[size_t(a)], int(members.size()));
            if (k <= 0 || members.empty()) continue;
            const Vec3 U = MeshCard::axisU(a);
            const Vec3 V = MeshCard::axisV(a);
            const Vec3 D = MeshCard::axisDirection(a);

            // K-means IN THE CARD'S OWN FRAME — (u, v, depth * kClusterDepthWeight)
            // — inside the bucket (the normal term IS the bucket).
            // Farthest-point seeding: deterministic, and it puts the first
            // centres where the spread is instead of where an index happens to
            // start.
            std::vector<Vec3> feature(members.size());
            for (size_t m = 0; m < members.size(); ++m) {
                const Vec3 &p = surfels[members[m]].pos;
                feature[m] = Vec3(Vec3::dotProduct(p, U), Vec3::dotProduct(p, V),
                                  Vec3::dotProduct(p, D) * kClusterDepthWeight);
            }
            std::vector<Vec3> centroid;
            {
                Vec3 mean;
                for (const Vec3 &f : feature) mean = mean + f;
                mean = mean / float(feature.size());
                size_t first = 0; float best = -1.0f;
                for (size_t m = 0; m < feature.size(); ++m) {
                    const float d = (feature[m] - mean).lengthSquared();
                    if (d > best) { best = d; first = m; }
                }
                std::vector<size_t> centres{ first };
                std::vector<float> nearest(feature.size(), FLT_MAX);
                while (int(centres.size()) < k) {
                    size_t pick = 0; float far = -1.0f;
                    for (size_t m = 0; m < feature.size(); ++m) {
                        const float d = (feature[m] - feature[centres.back()]).lengthSquared();
                        nearest[m] = std::min(nearest[m], d);
                        if (nearest[m] > far) { far = nearest[m]; pick = m; }
                    }
                    if (!(far > 0.0f)) break;
                    centres.push_back(pick);
                }
                for (size_t m : centres) centroid.push_back(feature[m]);
                // The ADDED centre goes where the misses are.
                if (seenHint && centroid.size() >= 2) {
                    Vec3 missMean; int missCount = 0;
                    for (size_t m = 0; m < members.size(); ++m)
                        if (!(*seenHint)[members[m]]) { missMean = missMean + feature[m]; ++missCount; }
                    if (missCount > 0) centroid.back() = missMean / float(missCount);
                }
            }

            std::vector<int> assign(members.size(), 0);
            for (int iter = 0; iter < kKMeansIterations; ++iter) {
                bool moved = false;
                for (size_t m = 0; m < members.size(); ++m) {
                    int pick = 0; float bestD = FLT_MAX;
                    for (size_t c = 0; c < centroid.size(); ++c) {
                        const float d = (feature[m] - centroid[c]).lengthSquared();
                        if (d < bestD) { bestD = d; pick = int(c); }
                    }
                    if (assign[m] != pick) { assign[m] = pick; moved = true; }
                }
                std::vector<Vec3> sum(centroid.size());
                std::vector<int> count(centroid.size(), 0);
                for (size_t m = 0; m < members.size(); ++m) {
                    sum[size_t(assign[m])] = sum[size_t(assign[m])] + feature[m];
                    ++count[size_t(assign[m])];
                }
                for (size_t c = 0; c < centroid.size(); ++c)
                    if (count[c] > 0) centroid[c] = sum[c] / float(count[c]);
                if (!moved) break;
            }

            for (size_t c = 0; c < centroid.size(); ++c) {
                float uMin = FLT_MAX, uMax = -FLT_MAX, vMin = FLT_MAX, vMax = -FLT_MAX;
                float dMin = FLT_MAX, dMax = -FLT_MAX;
                int n = 0;
                for (size_t m = 0; m < members.size(); ++m) {
                    if (assign[m] != int(c)) continue;
                    const Vec3 &p = surfels[members[m]].pos;
                    const float u = Vec3::dotProduct(p, U);
                    const float v = Vec3::dotProduct(p, V);
                    const float d = Vec3::dotProduct(p, D);
                    uMin = std::min(uMin, u); uMax = std::max(uMax, u);
                    vMin = std::min(vMin, v); vMax = std::max(vMax, v);
                    dMin = std::min(dMin, d); dMax = std::max(dMax, d);
                    ++n;
                }
                if (n == 0) continue;
                list.append(fromBounds(a, uMin, uMax, vMin, vMax, dMin, dMax, margin, lo, hi));
            }
        }
        return list;
    };

    // One card each, biggest buckets first while the budget allows. A face with
    // no card at all is a hole; a face with a second card is a luxury.
    std::vector<int> quota(MeshCard::kAxisCount, 0);
    {
        std::vector<int> order;
        for (int a = 0; a < MeshCard::kAxisCount; ++a)
            if (!bucket[size_t(a)].empty()) order.push_back(a);
        std::stable_sort(order.begin(), order.end(), [&](int l, int r) {
            return bucket[size_t(l)].size() > bucket[size_t(r)].size();
        });
        int left = maxCards;
        for (int a : order) { if (left <= 0) break; quota[size_t(a)] = 1; --left; }
    }

    QVector<MeshCard> clustered = listFor(quota);
    std::vector<char> seen(surfels.size(), 0);
    float coverage = measureList(&clustered, surfels, positions, posComps, *rasterIndices,
                                 extraTolerance, &seen);
    // A bucket whose split did not pay is not tried again — but the LOOP does
    // not stop there: another axis may still have surface nothing sees. Each
    // round costs exactly one measurement, and there are at most six refusals
    // plus (maxCards - buckets) acceptances, so the whole loop is bounded.
    std::vector<char> exhausted(MeshCard::kAxisCount, 0);
    while (clustered.size() < maxCards && coverage < kGoodCoverage) {
        // The bucket holding the most surfels nothing sees yet.
        int worst = -1; size_t missing = 0;
        for (int a = 0; a < MeshCard::kAxisCount; ++a) {
            if (exhausted[size_t(a)]) continue;
            if (quota[size_t(a)] >= int(bucket[size_t(a)].size())) continue;
            size_t miss = 0;
            for (size_t i : bucket[size_t(a)]) miss += size_t(seen[i] == 0);
            if (miss > missing) { missing = miss; worst = a; }
        }
        if (worst < 0 || missing == 0) break;
        std::vector<int> tryQuota = quota;
        ++tryQuota[size_t(worst)];
        QVector<MeshCard> candidate = listFor(tryQuota, &seen);
        std::vector<char> candidateSeen(surfels.size(), 0);
        const float gained = measureList(&candidate, surfels, positions, posComps, *rasterIndices,
                                         extraTolerance, &candidateSeen);
        if (gained <= coverage + kMinSplitGain) { exhausted[size_t(worst)] = 1; continue; }
        quota = tryQuota;
        clustered = candidate;
        seen = candidateSeen;
        coverage = gained;
    }


    // ---- 4. the 6-face box, ALWAYS measured against the clustered list ---
    //
    // Lumen falls back to the box for "meshes that yield few clusters". Rather
    // than guess what "few" is, both lists are measured the same way and the
    // better one wins — and the box is measured even when the clustered list
    // already passes, because it is sometimes BOTH better and smaller (a torus:
    // six box cards see 0.983 of it, seven clustered ones 0.909). Measuring it
    // only on a failure made the answer non-monotonic in the budget, which a
    // user raising `maxCards` would rightly read as a bug.
    QVector<MeshCard> best = clustered;
    float bestCoverage = coverage;
    if (maxCards >= MeshCard::kAxisCount) {
        QVector<MeshCard> box;
        for (int a = 0; a < MeshCard::kAxisCount; ++a) {
            float uMin, uMax, vMin, vMax, dMin, dMax;
            planeLimits(a, lo, hi, &uMin, &uMax, &vMin, &vMax, &dMin, &dMax);
            box.append(fromBounds(a, uMin, uMax, vMin, vMax, dMin, dMax, margin, lo, hi));
        }
        const float boxCoverage = measureList(&box, surfels, positions, posComps, *rasterIndices,
                                              extraTolerance);
        // Strictly better coverage wins; so does the same coverage for FEWER
        // cards, because a card is a capture pass forever after.
        if (boxCoverage > bestCoverage
            || (boxCoverage >= bestCoverage - kMinSplitGain && box.size() < best.size())) {
            best = box;
            bestCoverage = boxCoverage;
        }
    }

    // ---- the LOD level each card's texel picks (ATOM-2's rule) ----------
    for (MeshCard &card : best) {
        const float texel = std::max(card.halfU, card.halfV) * 2.0f / float(kCaptureResolution);
        card.lodLevel = quint8(levelForTexel(mesh, texel));
    }

    // ORDER IS PART OF THE OUTPUT (the bake must be byte-deterministic): axis
    // first, then the card's own origin, so two runs of the same mesh produce
    // the same list in the same order whatever the clustering visited first.
    std::stable_sort(best.begin(), best.end(), [](const MeshCard &l, const MeshCard &r) {
        if (l.axis != r.axis) return l.axis < r.axis;
        if (l.origin.x() != r.origin.x()) return l.origin.x() < r.origin.x();
        if (l.origin.y() != r.origin.y()) return l.origin.y() < r.origin.y();
        return l.origin.z() < r.origin.z();
    });

    mesh->cards = best;
    mesh->cardCoverage = bestCoverage;
}

}   // namespace cards

}   // namespace

void MeshBake::buildLodChain(const MeshPtr &mesh, QVector<LodLevelTerms> *terms)
{
    lodchain::build(mesh, terms);
}

void MeshBake::buildCards(const MeshPtr &mesh, int maxCards) { cards::build(mesh, maxCards); }

int MeshBake::cardCaptureResolution() { return cards::kCaptureResolution; }

namespace
{
/// The mesh's vertex data and level-0 index list as the DAG builder reads them; false when the mesh gets no DAG
/// (skinned, not triangles, malformed).
bool dagInput(const MeshPtr &mesh, atomdag::MeshView *view, std::vector<unsigned> *base)
{
    if (mesh.isNull() || !mesh->getSkeleton().isNull() || mesh->primitiveMode != PrimitiveMode::Triangles) return false;
    int posComps = 3; size_t nv = 0;
    const float *positions = lodchain::attribData(mesh, VertexAttribUsage::Position, &posComps, &nv);
    if (!positions || posComps < 3 || nv < 3) return false;
    const IndexBufferPtr ib = mesh->getIndexBuffer();
    if (ib.isNull() || !ib->data || ib->dataSize <= 0) return false;
    base->assign(reinterpret_cast<const unsigned *>(ib->data),
                 reinterpret_cast<const unsigned *>(ib->data) + size_t(ib->dataSize) / sizeof(unsigned));
    if (base->size() < 3 || base->size() % 3 != 0) return false;
    for (unsigned i : *base) if (size_t(i) >= nv) return false;
    view->positions = positions;
    view->posComps = posComps;
    view->nv = nv;
    int nrmComps = 3; size_t nrmCount = 0;
    const float *normals = lodchain::attribData(mesh, VertexAttribUsage::Normal, &nrmComps, &nrmCount);
    if (normals && nrmCount >= nv && nrmComps >= 3) { view->normals = normals; view->nrmComps = nrmComps; }
    int uvComps = 2; size_t uvCount = 0;
    const float *uvs = lodchain::attribData(mesh, VertexAttribUsage::TexCoord0, &uvComps, &uvCount);
    if (uvs && uvCount >= nv && uvComps >= 2) { view->uvs = uvs; view->uvComps = uvComps; }
    return true;
}

/// THE CARD QUADS' VERTICES INTO THE MESH (D9): every per-vertex float buffer grows by the appended corners — the
/// position, the normal and the first UV set's u and v as the build placed them, every other component and buffer (the
/// second UV set, the tangent, the bitangent's handedness) copied from the corner's source vertex. The quads lie inside
/// their sources' box, so the mesh's box and sphere do not move — checked here, loud if ever not.
void applyAppended(const MeshPtr &mesh, const atomdag::Appended &a)
{
    if (mesh.isNull() || a.count() == 0) return;
    for (const VertexBufferPtr &vb : mesh->getVertexBuffers()) {
        if (!vb || !vb->data || vb->dataSize <= 0) continue;
        const QList<VertexAttribute> attribs = vb->vertexLayout.getAttribs();
        if (attribs.size() != 1) continue;
        const VertexAttribute &at = attribs.first();
        const size_t comps = size_t(at.count > 0 ? at.count : 3);
        const size_t nv = size_t(vb->dataSize) / (sizeof(float) * comps);
        if (nv != a.nv0) continue;   // not a per-vertex float buffer of this mesh
        const float *src = reinterpret_cast<const float *>(vb->data);
        std::vector<float> data(src, src + nv * comps);
        data.reserve((nv + a.count()) * comps);
        for (size_t i = 0; i < a.count(); ++i) {
            const size_t base = data.size();
            data.insert(data.end(), src + size_t(a.source[i]) * comps, src + (size_t(a.source[i]) + 1) * comps);
            const atomdag::CardCorner &c = a.corners[i];
            const float *placed = at.usage == VertexAttribUsage::Position ? c.pos
                                : at.usage == VertexAttribUsage::Normal ? c.nrm
                                : at.usage == VertexAttribUsage::TexCoord0 ? c.uv : nullptr;
            const size_t placedComps = at.usage == VertexAttribUsage::TexCoord0 ? 2u : 3u;
            if (placed) for (size_t k = 0; k < std::min(comps, placedComps); ++k) data[base + k] = placed[k];
        }
        vb->setData(data.data(), unsigned(data.size() * sizeof(float)));
    }
    mesh->numVerts = int(a.nv0 + a.count());
    int outside = 0;
    for (const atomdag::CardCorner &c : a.corners)
        for (int k = 0; k < 3; ++k)
            if (c.pos[k] < mesh->aabb.getMin()[k] || c.pos[k] > mesh->aabb.getMax()[k]) { ++outside; break; }
    if (outside)
        irisLog(QStringLiteral("mesh bake: DEFECT %1 card-quad corners outside the mesh's box (it was not grown)").arg(outside));
}

/// The DAG of `mesh` without growing its buffers: the bake's stages read the mesh concurrently, so the card quads'
/// vertices land after them (bakeStages); every other caller applies them at once.
void buildDagDeferred(const MeshPtr &mesh, MeshBake::ClusterDagStats *stats, const MeshBake::ClusterDagOptions &options,
                      atomdag::Appended *appended)
{
    if (stats) *stats = MeshBake::ClusterDagStats();
    *appended = atomdag::Appended();
    if (mesh.isNull()) return;
    mesh->clusterDag = MeshClusterDag();
    atomdag::MeshView view;
    std::vector<unsigned> base;
    if (!dagInput(mesh, &view, &base)) return;
    if (!atomdag::build(view, base, &mesh->clusterDag, stats, options, appended))
        irisLog(QStringLiteral("mesh bake: cluster DAG NOT BUILT (%1 triangles) — the mesh draws level 0").arg(base.size() / 3));
}
}   // namespace

void MeshBake::buildClusterDag(const MeshPtr &mesh, ClusterDagStats *stats, const ClusterDagOptions &options)
{
    atomdag::Appended appended;
    buildDagDeferred(mesh, stats, options, &appended);
    applyAppended(mesh, appended);
}

QVector<MeshBake::DagDriftRow> MeshBake::dagDrift(const MeshPtr &mesh)
{
    QVector<DagDriftRow> rows;
    atomdag::MeshView view;
    std::vector<unsigned> base;
    if (!dagInput(mesh, &view, &base) || mesh->clusterDag.isEmpty()) return rows;
    const MeshClusterDag &dag = mesh->clusterDag;
    int maxDepth = 0;
    for (const MeshClusterDag::Group &g : dag.groups) maxDepth = std::max(maxDepth, int(g.depth));
    Vec3 lo = view.position(base[0]), hi = lo;
    for (unsigned v : base) {
        const Vec3 p = view.position(v);
        lo = Vec3(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
        hi = Vec3(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
    }
    surface::TriangleGrid baseGrid;
    baseGrid.build(view.positions, view.posComps, base, lo, hi, lodchain::boundGridRes(base.size() / 3));
    const atomdag::Islands islands = atomdag::findIslands(view.positions, view.posComps, view.nv, base);
    const std::vector<unsigned> baseSet = lodchain::vertexSet(base.data(), base.size());
    surface::Queries queries(static_cast<size_t>(bakepool::width()));
    for (int d = 0; d <= maxDepth; ++d) {
        // THE UNIFORM CUT AT DEPTH d: every cluster born at or before d whose own group forms at or after d.
        std::vector<unsigned> level;
        float stored = 0.0f;
        for (const MeshClusterDag::Cluster &c : dag.clusters) {
            const int born = c.refined < 0 ? 0 : int(dag.groups.at(c.refined).depth) + 1;
            const int dies = int(dag.groups.at(c.group).depth);
            if (born > d || dies < d) continue;
            if (c.refined >= 0) stored = std::max(stored, dag.groups.at(c.refined).error);
            for (int t = 0; t < int(c.triangleCount); ++t)
                for (int k = 0; k < 3; ++k) level.push_back(dag.vertexOf(c, t, k));
        }
        DagDriftRow row;
        row.depth = d;
        row.triangles = int(level.size() / 3);
        row.stored = stored;
        if (d > 0) {   // (an empty level too: every level-0 point is then charged its island's extent)
            surface::TriangleGrid grid;
            grid.build(view.positions, view.posComps, level, lo, hi, lodchain::boundGridRes(std::max<size_t>(level.size() / 3, 1)));
            float drift = lodchain::removedVertexDistance(view.positions, view.posComps, baseSet, level, grid, &islands,
                                                          queries);
            drift = lodchain::facetDistance(view.positions, view.posComps, base, level, grid, &islands,
                                            lodchain::FacetPattern::Reference, queries, drift);
            drift = lodchain::facetDistance(view.positions, view.posComps, level, base, baseGrid, nullptr,
                                            lodchain::FacetPattern::Reference, queries, drift);
            row.drift = drift;
        }
        rows.append(row);
    }
    return rows;
}


bool MeshBake::checkLodBounds(const MeshPtr &mesh, int densityMultiple, double *worstRatioOut,
                              QVector<float> *referenceOut)
{
    if (worstRatioOut) *worstRatioOut = 0.0;
    if (referenceOut) referenceOut->clear();
    if (mesh.isNull() || mesh->lodIndices.isEmpty()) return true;
    if (mesh->lodBounds.size() != mesh->lodIndices.size()) return false;
    int posComps = 3; size_t nv = 0;
    const float *positions =
        lodchain::attribData(mesh, VertexAttribUsage::Position, &posComps, &nv);
    if (!positions || posComps < 3 || nv < 3) return false;
    const IndexBufferPtr ib = mesh->getIndexBuffer();
    if (ib.isNull() || !ib->data || ib->dataSize <= 0) return false;
    std::vector<unsigned> base(reinterpret_cast<const unsigned *>(ib->data),
                               reinterpret_cast<const unsigned *>(ib->data) +
                                   size_t(ib->dataSize) / sizeof(unsigned));
    if (base.size() < 3 || base.size() % 3 != 0) return false;

    Vec3 lo(positions[0], positions[1], positions[2]), hi = lo;
    for (size_t v = 0; v < nv; ++v) {
        const float *p = positions + v * size_t(posComps);
        lo = Vec3(std::min(lo.x(), p[0]), std::min(lo.y(), p[1]), std::min(lo.z(), p[2]));
        hi = Vec3(std::max(hi.x(), p[0]), std::max(hi.y(), p[1]), std::max(hi.z(), p[2]));
    }
    surface::TriangleGrid baseGrid;
    baseGrid.build(positions, posComps, base, lo, hi, lodchain::boundGridRes(base.size() / 3));
    const lodchain::Islands islands = lodchain::findIslands(positions, posComps, nv, base);
    surface::Queries queries(static_cast<size_t>(bakepool::width()));
    const size_t dense =
        size_t(std::max(1, densityMultiple)) *
        size_t(base.size() / 3 > size_t(lodchain::kBoundBigTriangles)
                   ? lodchain::kBoundSamplesBig : lodchain::kBoundSamples);

    for (int k = 0; k < mesh->lodIndices.size(); ++k) {
        const QVector<quint32> &levelQ = mesh->lodIndices.at(k);
        std::vector<unsigned> level(levelQ.constBegin(), levelQ.constEnd());
        surface::TriangleGrid levelGrid;
        levelGrid.build(positions, posComps, level, lo, hi,
                        lodchain::boundGridRes(level.size() / 3));
        // The same represented-surface rule the bake applies (island caps), at
        // `densityMultiple` times the samples and NO margin: the reference.
        // (F1) and the PER-FACET reference: every level-0 facet at its corners,
        // edge midpoints, centroid and the centroids of its 1-to-4 split (a facet
        // the level kept contributes zero), and every level facet likewise against
        // level 0 — strictly denser than the bake's facet points.
        float area = 0.0f;
        const float exact = lodchain::twoSidedDistance(positions, posComps, level, levelGrid,
                                                       base, baseGrid, dense, &area, &islands, queries);
        float facets = lodchain::facetDistance(positions, posComps, base, level, levelGrid,
                                               &islands, lodchain::FacetPattern::Reference, queries);
        facets = lodchain::facetDistance(positions, posComps, level, base, baseGrid, nullptr,
                                         lodchain::FacetPattern::Reference, queries, facets);
        const float measured = std::max(std::max(area, exact), facets);
        if (referenceOut) referenceOut->append(measured);
        // A float comparison of two lengths measured the same way: one part in a
        // million of the stored value is the round-off, not a tolerance on the
        // claim.
        const float stored = mesh->lodBounds.at(k);
        if (worstRatioOut && stored > 0.0f)
            *worstRatioOut = std::max(*worstRatioOut, double(measured) / double(stored));
        if (measured > stored * (1.0f + 1e-6f) + 1e-9f) return false;
    }
    return true;
}


MeshBake::Model MeshBake::buildFromScene(const SceneSource &source, const QString &filePath,
                                         const QString &fingerprint, const QString &extractDir,
                                         const ImportTransform &xf)
{
    return buildFromScene(source.scene(), filePath, fingerprint, extractDir, xf);
}

namespace
{

/// A CANCELLED BAKE (STAGE-CLOSE-TRIAGE-1 F5): thrown at a cancel point, on the caller's thread or on
/// a pool thread (bakepool rethrows it on the caller), and answered by buildFromScene with an invalid
/// model. Never thrown when the flag stays false, so an uncancelled bake is unchanged.
struct BakeCancelled : std::exception
{
    const char *what() const noexcept override { return "cancelled"; }
};
inline void cancelPoint(const std::atomic<bool> *cancel)
{
    if (cancel && cancel->load(std::memory_order_relaxed)) throw BakeCancelled();
}
/// The parse's cancel point: a false answer from Update() aborts the import — the vendored assimp
/// honours it only through our patch 0010 (thirdparty/assimp-patches): after the format's read and
/// before each post-processing step, freeing the partial scene (ReadFile returns null). A cancel
/// raised DURING a format's read waits for that read to finish. The Importer owns and deletes it.
/// KNOWN LIMIT (read 6 I1, accepted for STAGE-CLOSE-TRIAGE-1): assimp's format readers have no cancel
/// point of their own, so a quit while a very large file is being READ (a Bistro-class FBX) waits for
/// the read; past the shutdown's 20 s bound the forced-exit watchdog ends the process (exit 86). Nothing
/// is committed in that case — the bake's temp output dies with the process. Closing it needs cancel
/// checks inside each reader (a further assimp patch), not taken here.
class CancelProgress : public Assimp::ProgressHandler
{
public:
    explicit CancelProgress(const std::atomic<bool> *cancel) : mCancel(cancel) {}
    bool Update(float) override { return !(mCancel && mCancel->load(std::memory_order_relaxed)); }

private:
    const std::atomic<bool> *mCancel;
};

/// EVERY MESH'S PRODUCTS, CONCURRENTLY (IMPORT-SPEED-1). Per mesh, two chains of
/// work that share nothing but the mesh's read-only level 0:
///
///   * ATOM stage 1: the LOD chain, then SURFACE-CACHE phase 1's card list (a card
///     names the level its texel picks), then ATOM P2's signed distance field (its
///     cell may not be finer than level 1's measured bound) — that order is
///     load-bearing, so the three run one after another in one task;
///   * ATOM stage 2: the cluster DAG, which simplifies level 0 itself and reads
///     nothing the other three write, in its own task.
///
/// Every task writes only its own mesh's own fields, and each stage's output is a
/// function of its inputs alone (`bakepool`), so the blob does not depend on which
/// task ran first or on how many threads ran them. The fallback parse path (a
/// library with no bake yet) gets none of these — the same "no LOD" behaviour the
/// tree has always had, and one more reason a bake is worth having.
///
/// ONE LOG LINE per mesh that gets a DAG, in mesh order after all tasks, with the
/// bake's two FIX COUNTS: a group error raised to a child's, or a sphere grown to
/// contain a child's, is the bake correcting its own measurement, and a count that
/// is suddenly large is the thing to see (shipped content: 0-3 per mesh, 17 spheres
/// on the dragon).
void bakeStages(MeshBake::Model &model, const QString &filePath, int maxCards,
                const std::atomic<bool> *cancel)
{
    using Clock = std::chrono::steady_clock;
    const auto msSince = [](Clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    };
    const size_t meshes = size_t(model.meshes.size());
    model.stageMs = QVector<MeshBake::Model::StageMs>(int(meshes));
    model.bakeThreads = bakepool::width();
    std::vector<MeshBake::ClusterDagStats> dagStats(meshes);
    std::vector<atomdag::Appended> appended(meshes);   // the card quads' vertices, applied after every stage
    const Clock::time_point start = Clock::now();
    // Tasks [0, meshes) are the chains (the longest work, handed out first), then
    // [meshes, 2 * meshes) the DAGs.
    bakepool::forRange(bakepool::Range(meshes * 2, 1), bakepool::width(),
                       [&](size_t, size_t begin, size_t end, int) {
        for (size_t task = begin; task < end; ++task) {
            const size_t i = task % meshes;
            const MeshPtr &mesh = model.meshes.at(int(i));
            MeshBake::Model::StageMs &ms = model.stageMs[int(i)];
            if (task < meshes) {
                cancelPoint(cancel);
                Clock::time_point t = Clock::now();
                MeshBake::buildLodChain(mesh);
                ms.lodChain = msSince(t);
                cancelPoint(cancel);
                t = Clock::now();
                MeshBake::buildCards(mesh, maxCards);
                ms.cards = msSince(t);
            } else {
                cancelPoint(cancel);
                const Clock::time_point t = Clock::now();
                buildDagDeferred(mesh, &dagStats[i], MeshBake::ClusterDagOptions(), &appended[i]);
                ms.dag = msSince(t);
            }
        }
    });
    for (size_t i = 0; i < meshes; ++i) applyAppended(model.meshes.at(int(i)), appended[i]);
    // THE DEPTH CAP, named (the chain closed with the empty top over its remaining pool — a DAG all the same).
    for (size_t i = 0; i < meshes; ++i)
        if (dagStats[i].depthCapHit)
            irisLog(QStringLiteral("mesh bake: WARNING %1 mesh %2 (%3 clusters, %4 groups) met the cluster DAG's depth cap of %5 — "
                                   "closed with the empty top over the clusters it still held")
                        .arg(filePath).arg(i).arg(dagStats[i].clusters).arg(dagStats[i].groups).arg(atomdag::kMaxLevels));
    model.meshesMs = msSince(start);
#if defined(__GLIBC__)
    // THE POOL'S FREED MEMORY GOES BACK. glibc gives every thread its own malloc
    // arena and keeps what a thread freed for that thread's next allocation, so
    // after a bake the pool's twenty arenas would sit on their share of the
    // working set (measured on the owner's scan: the bake's transient peak is
    // ~400 MB above the serial bake's, most of it scratch that is already freed
    // here). One trim returns the free pages of every arena to the system.
    malloc_trim(0);
#endif
    for (size_t i = 0; i < meshes; ++i) {
        const MeshBake::ClusterDagStats &st = dagStats[i];
        if (st.clusters > 0)
            irisLog(QStringLiteral("mesh bake: cluster DAG %1 (mesh %2): %3 clusters, %4 groups, depth %5; "
                                   "%6 ms (leaves %7, groups %8, reduce %9); ladder: %10 reductions, %11 carried, "
                                   "%12 retries, %13 pieces pruned")
                        .arg(QFileInfo(filePath).fileName()).arg(i).arg(st.clusters)
                        .arg(st.groups).arg(st.depth)
                        .arg(st.msTotal, 0, 'f', 1).arg(st.msLeaves, 0, 'f', 1).arg(st.msGroups, 0, 'f', 1)
                        .arg(st.msReduce, 0, 'f', 1).arg(st.reductions).arg(st.carried).arg(st.retries)
                        .arg(st.prunedPieces));
    }
}

}   // namespace

void MeshBake::setBakeThreads(int threads) { bakepool::gWidth.store(std::max(threads, 0)); }
int MeshBake::bakePoolThreads() { return bakepool::Pool::instance().threads(); }

int MeshBake::bakeThreads() { return bakepool::width(); }

QString MeshBake::Model::stageSummary() const
{
    QStringList parts;
    for (int i = 0; i < meshes.size() && i < stageMs.size(); ++i) {
        const MeshPtr &mesh = meshes.at(i);
        const IndexBufferPtr ib = mesh ? mesh->getIndexBuffer() : IndexBufferPtr();
        const qint64 tris = ib && ib->data ? qint64(ib->dataSize) / qint64(sizeof(unsigned) * 3) : 0;
        const StageMs &ms = stageMs.at(i);
        parts << QStringLiteral("m%1 %2t lod %3 cards %4 dag %5")
                     .arg(i).arg(tris).arg(ms.lodChain, 0, 'f', 0).arg(ms.cards, 0, 'f', 0)
                     .arg(ms.dag, 0, 'f', 0);
    }
    return QStringLiteral("meshes %1 ms on %2 threads [%3]")
        .arg(meshesMs, 0, 'f', 0).arg(bakeThreads).arg(parts.join(QStringLiteral(" | ")));
}

MeshBake::Model MeshBake::buildFromScene(const aiScene *scene, const QString &filePath,
                                         const QString &fingerprint, const QString &extractDir,
                                         const ImportTransform &xf, const std::atomic<bool> *cancel)
{
    // A BAKE THAT THROWS IS NO BAKE (IMPORT-SPEED-1 F1): an out-of-memory on a huge
    // asset, or anything a stage throws on a pool thread (rethrown here by
    // `bakepool::Pool::run`), is logged and answered with an invalid model — the
    // import keeps its source and the open path parses instead, as for any bake
    // that could not be written. Never an exception out of the import's worker.
    try {
        return buildFromSceneUnguarded(scene, filePath, fingerprint, extractDir, xf, cancel);
    } catch (const BakeCancelled &) {
        irisLog(QStringLiteral("mesh bake: %1 cancelled").arg(QFileInfo(filePath).fileName()));
        return Model();
    } catch (const std::exception &e) {
        irisLog(QStringLiteral("mesh bake: %1 could not be baked (%2); the open path will parse it instead")
                    .arg(QFileInfo(filePath).fileName(), QString::fromLocal8Bit(e.what())));
    } catch (...) {
        irisLog(QStringLiteral("mesh bake: %1 could not be baked (unknown exception); the open path will "
                               "parse it instead").arg(QFileInfo(filePath).fileName()));
    }
    return Model();
}

void MeshBake::failBakeAfterChunksForTest(int chunks) { bakepool::Pool::failAfterChunks(chunks); }

MeshBake::Model MeshBake::buildFromSceneUnguarded(const aiScene *scene, const QString &filePath,
                                                  const QString &fingerprint, const QString &extractDir,
                                                  const ImportTransform &xf, const std::atomic<bool> *cancel)
{
    Model model;
    if (!scene || scene->mNumMeshes == 0) return model;

    model.fingerprint = fingerprint;
    // Geometry: EXACTLY GraphicsHelper::loadAllMeshesFromAssimpScene, so the
    // baked meshes and the parsed ones are the same objects built by the same
    // code — not a second implementation that has to agree.
    QVector<int> materialFor(int(scene->mNumMeshes), -1);
    QHash<unsigned, int> materialIndexMap;
    const QString dir = QFileInfo(filePath).absoluteDir().absolutePath();
    // The meshes are built from the scene one after another (the Mesh
    // constructor reads assimp's scene); their STAGES then run concurrently
    // (`bakeStages`).
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh *m = scene->mMeshes[i];
        // THE TUNING SWITCHES (import/importsettings.h §4.3). They act on what
        // is BUILT, never on the parse: `skeleton:false` drops the skeleton AND
        // the bone index/weight arrays, so a rigged file bakes as static
        // geometry; the same two lines are what MeshNode::loadAsSceneFragment
        // does, because the bake and the parse fallback have to agree node for
        // node (tests/meshbake compares the two trees).
        cancelPoint(cancel);
        auto mesh = MeshPtr(new Mesh(const_cast<aiMesh *>(m), xf.skeleton));
        if (m->HasBones() && xf.skeleton) mesh->setSkeleton(Mesh::extractSkeleton(m, scene));
        model.meshes.append(mesh);
    }
    cancelPoint(cancel);   // between the mesh conversion and the stages
    bakeStages(model, filePath, xf.maxCards, cancel);
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh *m = scene->mMeshes[i];
        const unsigned aiMatIndex = m->mMaterialIndex;
        auto known = materialIndexMap.constFind(aiMatIndex);
        if (known != materialIndexMap.constEnd()) {
            materialFor[int(i)] = known.value();
            continue;
        }
        MeshMaterialData data;
        if (xf.materials && aiMatIndex < scene->mNumMaterials)
            MaterialHelper::extractMaterialData(scene, scene->mMaterials[aiMatIndex],
                                                dir, data, extractDir, filePath, xf.faceCulling);
        // TEXTURE REFERENCES ARE REDUCED TO BARE FILE NAMES, for two reasons.
        // (1) Determinism: extractMaterialData resolves embedded textures to
        //     paths inside a per-run staging directory, so keeping them would
        //     make the same source bake to different bytes every time and
        //     assets.checkConsistency could never match the recorded object.
        // (2) Honesty: those paths do not exist at OPEN time anyway. Every
        //     consumer of a baked fragment re-points materials from the
        //     catalog definition (AssetHelper::updateNodeMaterial), and the
        //     extracted images are member Texture assets in the store.
        //
        // THE LIMIT OF (1), measured: a bake is a deterministic function of
        // the model bytes AND THE FILES BESIDE THEM, which is exactly what
        // every other product of the convert stage is. For a self-contained
        // model (glTF/GLB with embedded images, or anything untextured) it is
        // a pure function of the source and assets.checkConsistency is GREEN
        // (tests/meshbake asserts it). For a model with SIBLING files — an
        // .obj + .mtl + .png — checkConsistency re-converts from a staging
        // directory holding only the model, so the siblings are not there:
        // MaterialHelper::loadEmbeddedTexture then CLEARS a texture path that
        // does not resolve and the bake records "" where the import recorded
        // "wood.png". That check was already false for such an asset before
        // any of this existed — the .mtl and the .png cannot be re-derived
        // either (measured 2026-09-04 on an .obj+.mtl+.png: 3 expected
        // objects, 1 produced, consistent=false; with the bake it is 4 and 2).
        // The bake adds one more mismatched pair to an already-red report; it
        // does not create a failure class.
        const auto bareName = [](QString &s) { if (!s.isEmpty()) s = QFileInfo(s).fileName(); };
        bareName(data.diffuseTexture);
        bareName(data.specularTexture);
        bareName(data.normalTexture);
        bareName(data.hightTexture);
        bareName(data.baseColorTexture);
        bareName(data.metallicTexture);
        bareName(data.roughnessTexture);
        bareName(data.emissiveTexture);
        materialFor[int(i)] = model.materials.size();
        materialIndexMap.insert(aiMatIndex, model.materials.size());
        model.materials.append(data);
    }

    model.animations = Mesh::extractAnimations(scene, filePath);
    // `clips:false` / `clips:[names]`: filtered AFTER extraction, on the names
    // the rest of the app shows (extractAnimations uniquifies raw names, and a
    // filter written against the raw ones would miss).
    if (!xf.clips || !xf.clipNames.isEmpty()) {
        QMap<QString, SkeletalAnimationPtr> kept;
        for (auto it = model.animations.constBegin(); it != model.animations.constEnd(); ++it)
            if (xf.wantsClip(it.key())) kept.insert(it.key(), it.value());
        model.animations = kept;
    }

    // The facts of THIS parse, for every reader after the import (the
    // metadata backfill) — read off the same aiScene ModelSceneInfo reads.
    model.describe = ModelSceneInfo::fromScene(scene);

    // Same shortcut condition as both loadAsSceneFragment overloads.
    model.singleMesh = scene->mNumMeshes == 1 && scene->mMeshes[0]->mNumBones == 0;
    if (model.singleMesh) {
        BakedNode root;
        root.isMeshNode = true;
        root.meshIndex = 0;
        root.materialIndex = materialFor.value(0, -1);
        aiMatrix4x4 xform;
        if (findMeshNodeTransform(scene->mRootNode, 0, aiMatrix4x4(), xform)) {
            aiVector3D pos, scale;
            aiQuaternion rot;
            xform.Decompose(scale, rot, pos);
            root.pos = iris::Vec3(pos.x, pos.y, pos.z);
            root.scale = iris::Vec3(scale.x, scale.y, scale.z);
            root.rot = iris::Quat(rot.w, rot.x, rot.y, rot.z);
        }
        model.root = root;
    } else {
        model.root = bakeNode(scene, scene->mRootNode, materialFor);
    }

    model.valid = true;
    return model;
}

MeshBake::Model MeshBake::buildFromFile(const QString &filePath, const QString &fingerprint,
                                        const QString &extractDir, const ImportTransform &xf,
                                        const std::atomic<bool> *cancel)
{
    Assimp::Importer importer;
    // THE PARSE'S CANCEL POINT (F5): the importer owns the handler.
    if (cancel) importer.SetProgressHandler(new CancelProgress(cancel));
    // The lazy re-bake's parse, through the choke point with the ASSET's
    // import transform — the bake IS the transformed geometry, so a re-bake
    // that dropped the transform would quietly un-scale a library
    // (import/scenesource.h).
    const aiScene *scene = readSceneFile(importer, filePath, iris::ImportFlags::Canonical, xf);
    if (!scene) {
        if (cancel && cancel->load())
            irisLog(QStringLiteral("mesh bake: %1 cancelled during its parse").arg(QFileInfo(filePath).fileName()));
        else
            irisLog("mesh bake: assimp could not read " + filePath);
        return Model();
    }
    return buildFromScene(scene, filePath, fingerprint, extractDir, xf, cancel);
}

// ---- serialize / deserialize ----------------------------------------------

QByteArray MeshBake::serialize(const Model &model)
{
    QByteArray blob;
    QDataStream s(&blob, QIODevice::WriteOnly);
    configure(s);
    s << quint32(kMagic) << qint32(kFormatVersion) << model.fingerprint;
    // THE DESCRIBE BLOCK IN THE HEADER (SHIPPED-BAKES-1): the facts of the
    // import's parse, as one length-prefixed blob right after the key, so the
    // metadata backfill (readDescribe) reads them and stops — no geometry, no
    // LOD chain, no cards, no field is ever deserialized to describe a model.
    {
        QByteArray facts;
        QDataStream d(&facts, QIODevice::WriteOnly);
        configure(d);
        writeDescribe(d, model.describe);
        s << facts;
    }
    s << qint32(model.singleMesh ? 1 : 0);

    s << qint32(model.meshes.size());
    for (const MeshPtr &mesh : model.meshes) writeMesh(s, mesh);

    s << qint32(model.materials.size());
    for (const MeshMaterialData &m : model.materials) writeMaterial(s, m);

    writeAnimations(s, model.animations);
    writeNode(s, model.root);
    // A trailing sentinel: a truncated blob that happens to parse this far
    // still fails, and the reader never has to trust "no error so far".
    s << quint32(kMagic);
    return blob;
}

bool MeshBake::headerMatches(const QString &path, const QString &expectFingerprint)
{
    if (path.isEmpty() || expectFingerprint.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    // Magic + version + the length-prefixed fingerprint string sit at the
    // front (serialize()); 4KB covers them with room to spare and reads no
    // mesh payload.
    const QByteArray head = file.read(4096);
    file.close();
    if (head.size() < 16) return false;

    QDataStream s(head);
    configure(s);
    quint32 magic = 0;
    qint32 version = 0;
    QString fingerprint;
    s >> magic >> version;
    if (magic != kMagic || version != kFormatVersion) return false;
    s >> fingerprint;
    if (s.status() != QDataStream::Ok) return false;
    return fingerprint == expectFingerprint;
}

MeshBake::Model MeshBake::deserialize(const QByteArray &blob, const QString &expectFingerprint)
{
    Model model;
    if (blob.size() < 16) return model;

    QDataStream s(blob);
    configure(s);
    quint32 magic = 0;
    qint32 version = 0;
    s >> magic >> version;
    if (magic != kMagic || version != kFormatVersion) return model;
    s >> model.fingerprint;
    if (s.status() != QDataStream::Ok) return Model();
    if (!expectFingerprint.isEmpty() && model.fingerprint != expectFingerprint) return Model();
    {
        QByteArray facts;
        s >> facts;
        if (s.status() != QDataStream::Ok) return Model();
        QDataStream d(facts);
        configure(d);
        if (!::iris::readDescribe(d, model.describe)) return Model();
    }

    qint32 singleMesh = 0, meshCount = 0;
    s >> singleMesh >> meshCount;
    if (s.status() != QDataStream::Ok || meshCount < 0 || meshCount > 1000000) return Model();
    model.singleMesh = singleMesh != 0;

    bool ok = true;
    for (qint32 i = 0; i < meshCount; ++i) {
        MeshPtr mesh = readMesh(s, &ok);
        if (!ok || mesh.isNull() || s.status() != QDataStream::Ok) return Model();
        model.meshes.append(mesh);
    }

    qint32 materialCount = 0;
    s >> materialCount;
    if (s.status() != QDataStream::Ok || materialCount < 0 || materialCount > 1000000) return Model();
    for (qint32 i = 0; i < materialCount; ++i) {
        model.materials.append(readMaterial(s));
        if (s.status() != QDataStream::Ok) return Model();
    }

    if (!readAnimations(s, QString(), model.animations)) return Model();
    if (!readNode(s, model.root, 0)) return Model();

    quint32 tail = 0;
    s >> tail;
    if (s.status() != QDataStream::Ok || tail != kMagic) return Model();

    model.valid = true;
    return model;
}

bool MeshBake::readDescribe(const QString &path, ModelSceneInfo *out,
                            const QString &expectFingerprint)
{
    if (path.isEmpty() || !out) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    // SEQUENTIAL off the file: the header's magic, version, key and the facts
    // blob, then the read stops — the geometry behind it is never touched.
    QDataStream s(&file);
    configure(s);
    quint32 magic = 0;
    qint32 version = 0;
    QString fingerprint;
    s >> magic >> version;
    if (magic != kMagic || version != kFormatVersion) return false;
    s >> fingerprint;
    if (s.status() != QDataStream::Ok) return false;
    if (!expectFingerprint.isEmpty() && fingerprint != expectFingerprint) return false;
    QByteArray facts;
    s >> facts;
    if (s.status() != QDataStream::Ok) return false;
    QDataStream d(facts);
    configure(d);
    ModelSceneInfo info;
    if (!::iris::readDescribe(d, info) || !info.parsed) return false;
    *out = info;
    return true;
}

MeshBake::Model MeshBake::read(const QString &path, const QString &expectFingerprint)
{
    if (path.isEmpty()) return Model();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Model();
    const QByteArray blob = file.readAll();
    file.close();
    return deserialize(blob, expectFingerprint);
}

bool MeshBake::write(const QString &path, const Model &model, QString *errorOut)
{
    if (!model.valid) {
        if (errorOut) *errorOut = QStringLiteral("mesh bake: nothing to write");
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    // Atomic, for the same reason CAS objects are (STABILITY_PROGRAM_SPEC
    // lane 2): a half-written bake is worse than a missing one, because a
    // reader that trusts the file name draws nothing instead of falling back.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("mesh bake: cannot write %1").arg(path);
        return false;
    }
    const QByteArray blob = serialize(model);
    if (file.write(blob) != blob.size() || !file.commit()) {
        if (errorOut) *errorOut = QStringLiteral("mesh bake: short write to %1").arg(path);
        return false;
    }
    return true;
}

// ---- the clip bake (SHIPPED-BAKES-1) ---------------------------------------

QString MeshBake::clipFingerprintFor(const QString &sourceOid)
{
    if (sourceOid.isEmpty()) return QString();
    // The producer key, the CLIP read's own flags (not the canonical preset's)
    // and the strip's sample points — a change to any of them is a different
    // bake.
    QStringList fractions;
    for (double f : ClipFileInfo::stripFractions()) fractions << QString::number(f, 'g', 17);
    const QByteArray key = (producerId() + QStringLiteral("|clip|flags")
                            + QString::number(quint64(ImportFlags::ClipNamesOnly))
                            + QStringLiteral("|poses") + fractions.join(QLatin1Char(','))
                            + QLatin1Char('|') + sourceOid).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
}

QString MeshBake::clipFileNameFor(const QString &sourceOid)
{
    return QStringLiteral("%1-clip.jcb").arg(sourceOid.left(16));
}

MeshBake::Clip MeshBake::buildClipFromScene(const aiScene *scene, const QString &filePath,
                                            const QString &fingerprint)
{
    Clip clip;
    if (!scene) return clip;
    clip.info = ClipFileInfo::fromScene(scene, ClipFileInfo::stripFractions());
    if (!clip.info.parsed) return Clip();
    clip.animations = Mesh::extractAnimations(scene, filePath);
    clip.declaredUnitScale = ModelSceneInfo::fromScene(scene).declaredUnitScale;
    clip.fingerprint = fingerprint;
    clip.valid = true;
    return clip;
}

MeshBake::Clip MeshBake::buildClipFromFile(const QString &filePath, const QString &fingerprint)
{
    Assimp::Importer importer;
    // IDENTITY, ClipNamesOnly: the file's own declared unit applies (a clip's
    // keys are in its file's units) and nothing else — the rig's factor is
    // applied at read (clipAnimations).
    const aiScene *scene = readSceneFile(importer, filePath, ImportFlags::ClipNamesOnly);
    if (!scene) {
        irisLog("clip bake: assimp could not read " + filePath);
        return Clip();
    }
    return buildClipFromScene(scene, filePath, fingerprint);
}

QByteArray MeshBake::serializeClip(const Clip &clip)
{
    QByteArray blob;
    QDataStream s(&blob, QIODevice::WriteOnly);
    configure(s);
    s << quint32(kClipMagic) << qint32(kFormatVersion) << clip.fingerprint;
    writeClipInfo(s, clip.info);
    writeF64(s, clip.declaredUnitScale);
    writeAnimations(s, clip.animations);
    s << quint32(kClipMagic);
    return blob;
}

MeshBake::Clip MeshBake::deserializeClip(const QByteArray &blob, const QString &expectFingerprint)
{
    Clip clip;
    if (blob.size() < 16) return clip;
    QDataStream s(blob);
    configure(s);
    quint32 magic = 0;
    qint32 version = 0;
    s >> magic >> version;
    if (magic != kClipMagic || version != kFormatVersion) return clip;
    s >> clip.fingerprint;
    if (s.status() != QDataStream::Ok) return Clip();
    if (!expectFingerprint.isEmpty() && clip.fingerprint != expectFingerprint) return Clip();
    if (!readClipInfo(s, clip.info)) return Clip();
    clip.declaredUnitScale = readF64(s);
    if (!readAnimations(s, QString(), clip.animations)) return Clip();
    quint32 tail = 0;
    s >> tail;
    if (s.status() != QDataStream::Ok || tail != kClipMagic) return Clip();
    clip.valid = true;
    return clip;
}

MeshBake::Clip MeshBake::readClip(const QString &path, const QString &expectFingerprint)
{
    if (path.isEmpty()) return Clip();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Clip();
    const QByteArray blob = file.readAll();
    file.close();
    return deserializeClip(blob, expectFingerprint);
}

bool MeshBake::writeClip(const QString &path, const Clip &clip, QString *errorOut)
{
    if (!clip.valid) {
        if (errorOut) *errorOut = QStringLiteral("clip bake: nothing to write");
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("clip bake: cannot write %1").arg(path);
        return false;
    }
    const QByteArray blob = serializeClip(clip);
    if (file.write(blob) != blob.size() || !file.commit()) {
        if (errorOut) *errorOut = QStringLiteral("clip bake: short write to %1").arg(path);
        return false;
    }
    return true;
}

bool MeshBake::clipHeaderMatches(const QString &path, const QString &expectFingerprint)
{
    if (path.isEmpty() || expectFingerprint.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = file.read(4096);
    file.close();
    if (head.size() < 16) return false;
    QDataStream s(head);
    configure(s);
    quint32 magic = 0;
    qint32 version = 0;
    QString fingerprint;
    s >> magic >> version;
    if (magic != kClipMagic || version != kFormatVersion) return false;
    s >> fingerprint;
    return s.status() == QDataStream::Ok && fingerprint == expectFingerprint;
}

QMap<QString, SkeletalAnimationPtr> MeshBake::clipAnimations(const Clip &clip,
                                                             const ImportTransform &rig,
                                                             const QString &source)
{
    // THE RIG'S UNIFORM FACTOR, resolved exactly as readSceneFile resolves it
    // for a clip read under `rig` — with the CLIP file's own declaration
    // standing in for the probe parse a unit override would otherwise pay.
    double k = rig.scale;
    if (rig.overridesUnit())
        k = rig.globalScaleFactor(rig.declaredUnitScale > 0.0 ? rig.declaredUnitScale
                                                              : clip.declaredUnitScale);
    const float factor = float(k);
    const bool scaled = factor != 1.0f;

    QMap<QString, SkeletalAnimationPtr> out;
    for (auto it = clip.animations.constBegin(); it != clip.animations.constEnd(); ++it) {
        const SkeletalAnimationPtr &from = it.value();
        if (!from) continue;
        auto anim = SkeletalAnimation::create();
        anim->name = from->name;
        anim->source = source;
        anim->declaredLength = from->declaredLength;
        for (auto b = from->boneAnimations.constBegin(); b != from->boneAnimations.constEnd(); ++b) {
            auto *bone = new BoneAnimation();
            // ScaleProcess touches POSITION keys only — rotations and scales
            // come out as authored, so they are copied as they are.
            for (const auto *key : b.value()->posKeys->keys)
                bone->posKeys->addKey(scaled ? key->value * factor : key->value, key->time);
            for (const auto *key : b.value()->rotKeys->keys)
                bone->rotKeys->addKey(key->value, key->time);
            for (const auto *key : b.value()->scaleKeys->keys)
                bone->scaleKeys->addKey(key->value, key->time);
            anim->addBoneAnimation(b.key(), bone);
        }
        out.insert(it.key(), anim);
    }
    return out;
}

// ---- the fragment ----------------------------------------------------------

namespace
{

SceneNodePtr buildFragmentNode(
    const MeshBake::Model &model, const BakedNode &baked, const SceneNodePtr &rootBone,
    const QString &filePath,
    const std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData &data)> &createMaterialFunc)
{
    SceneNodePtr sceneNode;
    const auto attachMesh = [&](const BakedNode &n, const MeshNodePtr &meshNode) {
        if (n.meshIndex < 0 || n.meshIndex >= model.meshes.size()) return;
        MeshPtr mesh = model.meshes[n.meshIndex];
        meshNode->setMesh(mesh);
        meshNode->name = n.name;
        meshNode->meshPath = filePath;
        meshNode->meshIndex = n.meshIndex;
        if (n.materialIndex >= 0 && n.materialIndex < model.materials.size()) {
            MeshMaterialData data = model.materials[n.materialIndex];
            auto mat = createMaterialFunc(mesh, data);
            if (!!mat) meshNode->setMaterial(mat);
            // A TWO-SIDED source material draws both faces (CULL-MODE-1).
            if (data.twoSided) meshNode->setFaceCullingMode(FaceCullingMode::None);
        }
    };

    // ORDER IS _buildScene's, exactly: the synthesized mesh children of a
    // multi-mesh aiNode are added BEFORE the parent's transform is set (their
    // addChild keeps the transform, so it reads the parent's global transform),
    // and they get no rootBone.
    if (baked.isMeshNode) {
        auto meshNode = MeshNode::create();
        attachMesh(baked, meshNode);
        meshNode->rootBone = rootBone;
        sceneNode = meshNode;
    } else {
        sceneNode = SceneNodePtr(new SceneNode());
        sceneNode->name = baked.name;
        for (int i = 0; i < baked.meshChildCount && i < baked.children.size(); ++i) {
            auto meshNode = MeshNode::create();
            attachMesh(baked.children[i], meshNode);
            sceneNode->addChild(meshNode);
        }
    }

    sceneNode->setLocalPos(baked.pos);
    sceneNode->setLocalScale(baked.scale);
    sceneNode->setLocalRot(baked.rot);

    SceneNodePtr nextRootBone = rootBone;
    if (!nextRootBone) nextRootBone = sceneNode;

    for (int i = baked.meshChildCount; i < baked.children.size(); ++i) {
        auto built = buildFragmentNode(model, baked.children[i], nextRootBone,
                                       filePath, createMaterialFunc);
        sceneNode->addChild(built, false);
    }

    sceneNode->setAttached(true);
    return sceneNode;
}

}   // namespace

SceneNodePtr MeshBake::buildFragment(
    const Model &model, const QString &filePath,
    const std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData &data)> &createMaterialFunc)
{
    if (!model.valid || model.meshes.isEmpty()) return SceneNodePtr();

    // Clips are attached to the fragment root exactly as loadAsSceneFragment
    // does — FIRST clip active, not the alphabetically last one.
    const auto attachAnimations = [&](const SceneNodePtr &node) {
        for (auto it = model.animations.constBegin(); it != model.animations.constEnd(); ++it) {
            auto skelAnim = it.value();
            skelAnim->source = filePath;
            auto anim = Animation::createFromSkeletalAnimation(skelAnim);
            node->addAnimation(anim);
            if (node->getAnimations().size() == 1) node->setAnimation(anim);
        }
    };

    if (model.singleMesh) {
        auto node = MeshNode::create();
        MeshPtr mesh = model.meshes.first();
        attachAnimations(node);
        node->setMesh(mesh);
        node->meshPath = filePath;
        node->meshIndex = 0;
        if (model.root.materialIndex >= 0 && model.root.materialIndex < model.materials.size()) {
            MeshMaterialData data = model.materials[model.root.materialIndex];
            auto mat = createMaterialFunc(mesh, data);
            if (!!mat) node->setMaterial(mat);
            if (data.twoSided) node->setFaceCullingMode(FaceCullingMode::None);   // CULL-MODE-1
        }
        node->setLocalPos(model.root.pos);
        node->setLocalScale(model.root.scale);
        node->setLocalRot(model.root.rot);
        return node;
    }

    auto node = buildFragmentNode(model, model.root, SceneNodePtr(), filePath, createMaterialFunc);
    node->setAttached(false);   // root of the object shouldn't be attached
    attachAnimations(node);
    node->applyDefaultPose();
    return node;
}

}   // namespace iris
