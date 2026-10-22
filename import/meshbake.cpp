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
// v32 (2026-10-22, ATOM-2 stage 5): D10 ONE SIMPLIFIER — the whole-mesh level table (the casters, the voxeliser's
// level, the surface cache, the ray BLAS, the cards) is at most eight UNIFORM-DEPTH CUTS of the cluster DAG, each its
// bound the largest stored error at that depth; the old level chain (its simplifier loop, its sampled metric, its
// displacement lock and its stop rule) and the bound check are deleted. Every bake re-bakes (BAKEKEY-1).
constexpr int kFormatVersion = 32;
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



namespace drift {

/// THE DRIFT'S MEASUREMENT (MeshBake::dagDrift, a diagnostic — never stored): how far a uniform cut of the DAG lies from
/// level 0, over the one vertex buffer. (ATOM-2 stage 5: what is left of the old level chain's metric once the chain
/// became cuts of the DAG — the chain's build, its sampled area term, its lock and its stop rule are deleted.)

/// Cells per axis of a nearest-surface grid: about one triangle per cell (the cube root of the count), within [32, 128].
inline int gridRes(size_t triangles)
{
    const double cube = std::cbrt(double(std::max<size_t>(triangles, 1)));
    return std::clamp(int(cube), 32, 128);
}

/// Queries per chunk on the bake's threads.
constexpr size_t kQueryGrain = 64;

std::vector<unsigned> vertexSet(const unsigned *indices, size_t count)
{
    std::vector<unsigned> v(indices, indices + count);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

/// Every vertex of `baseSet` (level 0's, sorted unique) the cut `a` does not use, against the cut's surface, capped at
/// its island's extent: the maximum (folded in chunk order — the serial walk's answer).
float removedVertexDistance(const float *positions, int posComps, const std::vector<unsigned> &baseSet,
                            const std::vector<unsigned> &a, const surface::TriangleGrid &gridA,
                            const atomdag::Islands *islands, surface::Queries &queries)
{
    const std::vector<unsigned> kept = vertexSet(a.data(), a.size());
    std::vector<unsigned> removed;
    removed.reserve(baseSet.size());
    std::set_difference(baseSet.begin(), baseSet.end(), kept.begin(), kept.end(), std::back_inserter(removed));
    const bakepool::Range range(removed.size(), kQueryGrain);
    std::vector<float> chunks(range.count, 0.0f);
    bakepool::forRange(range, int(queries.size()), [&](size_t c, size_t begin, size_t end, int slot) {
        for (size_t i = begin; i < end; ++i) {
            const unsigned v = removed[i];
            const float *p = positions + size_t(v) * size_t(posComps);
            float d = gridA.closest(queries[size_t(slot)], Vec3(p[0], p[1], p[2]));
            if (islands) d = std::min(d, islands->capOf[v]);
            if (std::isfinite(d)) chunks[c] = std::max(chunks[c], d);
        }
    });
    float worst = 0.0f;
    for (float w : chunks) worst = std::max(worst, w);
    return worst;
}

/// Every facet of `from` that `against` does not have, queried at ten fixed points (its corners, centroid, edge
/// midpoints and corner sub-triangles' centroids) against `againstGrid`, level-0 points island-capped (`islands`,
/// null for the cut's own facets); each query stops at the running maximum. Returns max(worst, that).
float facetDistance(const float *positions, int posComps, const std::vector<unsigned> &from,
                    const std::vector<unsigned> &against, const surface::TriangleGrid &againstGrid,
                    const atomdag::Islands *islands, surface::Queries &queries, float worst)
{
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
    const bakepool::Range range(from.size() / 3, kQueryGrain);
    std::vector<float> chunks(range.count, worst);
    bakepool::forRange(range, int(queries.size()), [&](size_t chunk, size_t begin, size_t end, int slot) {
        surface::TriangleGrid::Query &query = queries[size_t(slot)];
        float local = worst;
        for (size_t t = begin; t < end; ++t) {
            std::array<unsigned, 3> f = { from[t * 3], from[t * 3 + 1], from[t * 3 + 2] };
            std::sort(f.begin(), f.end());
            if (std::binary_search(present.begin(), present.end(), f)) continue;
            const float cap = islands ? islands->capOf[from[t * 3]] : std::numeric_limits<float>::infinity();
            if (!(cap > local)) continue;   // this island cannot exceed it
            const Vec3 a = at(from[t * 3]), b = at(from[t * 3 + 1]), c = at(from[t * 3 + 2]);
            const Vec3 pts[10] = { (a + b + c) * (1.0f / 3.0f), (a + b) * 0.5f, (b + c) * 0.5f, (c + a) * 0.5f, a, b, c,
                                   (a * 4.0f + b + c) * (1.0f / 6.0f), (b * 4.0f + c + a) * (1.0f / 6.0f),
                                   (c * 4.0f + a + b) * (1.0f / 6.0f) };
            for (const Vec3 &q : pts) {
                const float d = std::min(againstGrid.closest(query, q, nullptr, nullptr, nullptr, local), cap);
                if (std::isfinite(d) && d > local) local = d;
            }
        }
        chunks[chunk] = local;
    });
    for (float w : chunks) worst = std::max(worst, w);
    return worst;
}

}   // namespace drift

namespace
{
/// The first vertex buffer carrying `usage`, as floats: pointer, component count and how many vertices it holds. Null
/// when the mesh has no such buffer.
const float *attribData(const MeshPtr &mesh, VertexAttribUsage usage, int *componentsOut, size_t *countOut)
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
}   // namespace

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
    const float *positions = attribData(mesh, VertexAttribUsage::Position, &posComps, &nv);
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

void MeshBake::buildLevelTable(const MeshPtr &mesh)
{
    if (mesh.isNull()) return;
    mesh->lodIndices.clear();
    mesh->lodErrors.clear();
    mesh->lodBounds.clear();
    const MeshClusterDag &dag = mesh->clusterDag;
    if (dag.isEmpty()) return;
    int top = 0;
    for (const MeshClusterDag::Group &g : dag.groups) top = std::max(top, int(g.depth));
    // THE DEPTHS: 1 .. the root's (the empty top's own depth: its members are the root pool), evenly spaced past
    // kLevelTableMax.
    std::vector<int> depths;
    if (top <= kLevelTableMax) {
        for (int d = 1; d <= top; ++d) depths.push_back(d);
    } else {
        for (int i = 1; i <= kLevelTableMax; ++i) {
            const int d = int(std::lround(double(i) * double(top) / double(kLevelTableMax)));
            if (depths.empty() || d > depths.back()) depths.push_back(d);
        }
    }
    int previousTriangles = -1;
    for (int d : depths) {
        // THE UNIFORM CUT AT DEPTH d: every cluster born at or before d whose own group forms at or after d.
        QVector<quint32> level;
        float bound = 0.0f, estimate = 0.0f;
        for (const MeshClusterDag::Cluster &c : dag.clusters) {
            const int born = c.refined < 0 ? 0 : int(dag.groups.at(c.refined).depth) + 1;
            const int dies = c.group < 0 ? born : int(dag.groups.at(c.group).depth);
            if (born > d || dies < d) continue;
            if (c.refined >= 0) {
                bound = std::max(bound, dag.groups.at(c.refined).error);
                estimate = std::max(estimate, dag.groups.at(c.refined).estimate);
            }
            for (int t = 0; t < int(c.triangleCount); ++t)
                for (int k = 0; k < 3; ++k) level.append(quint32(dag.vertexOf(c, t, k)));
        }
        // a level is a reduction: something stored above zero and fewer triangles than the level before it
        const int triangles = int(level.size() / 3);
        if (!(bound > 0.0f) || triangles == 0 || (previousTriangles >= 0 && triangles >= previousTriangles)) continue;
        if (!mesh->lodBounds.isEmpty() && bound < mesh->lodBounds.last()) bound = mesh->lodBounds.last();   // never
        previousTriangles = triangles;
        mesh->lodIndices.append(level);
        mesh->lodBounds.append(bound);
        mesh->lodErrors.append(std::isfinite(estimate) && estimate >= 0.0f ? estimate : 0.0f);
    }
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
    const float *positions = attribData(mesh, VertexAttribUsage::Position, &posComps, &nv);
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
    const float *normals = attribData(mesh, VertexAttribUsage::Normal, &nrmComps, &nrmCount);
    if (normals && nrmCount >= nv && nrmComps >= 3) { view->normals = normals; view->nrmComps = nrmComps; }
    int uvComps = 2; size_t uvCount = 0;
    const float *uvs = attribData(mesh, VertexAttribUsage::TexCoord0, &uvComps, &uvCount);
    if (uvs && uvCount >= nv && uvComps >= 2) { view->uvs = uvs; view->uvComps = uvComps; }
    return true;
}

/// THE CARD QUADS' VERTICES INTO THE MESH (D9): every per-vertex float buffer grows by the appended corners — the
/// position, the normal and the first UV set's u and v as the build placed them, every other component and buffer (the
/// second UV set, the tangent, the bitangent's handedness) copied from the corner's source vertex — and the mesh's box
/// and sphere RECOMPUTED over them (a quad keeps its cards' summed area and may reach past level 0's box; picking and
/// the node bounds read the document's box, the engine takes its own from the uploaded positions).
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
    AABB box = mesh->aabb;
    for (const atomdag::CardCorner &c : a.corners) box.merge(Vec3(c.pos[0], c.pos[1], c.pos[2]));
    mesh->aabb = box;
    mesh->boundingSphere = box.getMinimalEnclosingSphere();
}

}   // namespace

void MeshBake::buildClusterDag(const MeshPtr &mesh, ClusterDagStats *stats, const ClusterDagOptions &options)
{
    if (stats) *stats = ClusterDagStats();
    if (mesh.isNull()) return;
    mesh->clusterDag = MeshClusterDag();
    atomdag::MeshView view;
    std::vector<unsigned> base;
    if (!dagInput(mesh, &view, &base)) return;
    atomdag::Appended appended;
    if (!atomdag::build(view, base, &mesh->clusterDag, stats, options, &appended))
        irisLog(QStringLiteral("mesh bake: cluster DAG NOT BUILT (%1 triangles) — the mesh draws level 0").arg(base.size() / 3));
    applyAppended(mesh, appended);   // the card quads' vertices (the DAG names them)
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
    baseGrid.build(view.positions, view.posComps, base, lo, hi, drift::gridRes(base.size() / 3));
    const atomdag::Islands islands = atomdag::findIslands(view.positions, view.posComps, view.nv, base);
    const std::vector<unsigned> baseSet = drift::vertexSet(base.data(), base.size());
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
            grid.build(view.positions, view.posComps, level, lo, hi, drift::gridRes(std::max<size_t>(level.size() / 3, 1)));
            float d = drift::removedVertexDistance(view.positions, view.posComps, baseSet, level, grid, &islands, queries);
            d = drift::facetDistance(view.positions, view.posComps, base, level, grid, &islands, queries, d);
            d = drift::facetDistance(view.positions, view.posComps, level, base, baseGrid, nullptr, queries, d);
            row.drift = d;
        }
        rows.append(row);
    }
    return rows;
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
void bakeStages(MeshBake::Model &model, const QString &filePath, const QStringList &meshNames, int maxCards,
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
    const Clock::time_point start = Clock::now();
    // ONE TASK PER MESH, its three products in order (ATOM-2 D10: the level table is cuts of the DAG, and the cards
    // read the table): the DAG (its card quads' vertices appended at once — nothing else reads this mesh meanwhile),
    // the level table, the cards. The biggest meshes are handed out first (each task is independent, so the order
    // moves no byte).
    std::vector<size_t> order(meshes);
    for (size_t i = 0; i < meshes; ++i) order[i] = i;
    const auto trianglesOf = [&](size_t i) {
        const IndexBufferPtr ib = model.meshes.at(int(i))->getIndexBuffer();
        return ib.isNull() || !ib->data ? size_t(0) : size_t(ib->dataSize) / 12u;
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return trianglesOf(a) > trianglesOf(b); });
    bakepool::forRange(bakepool::Range(meshes, 1), bakepool::width(),
                       [&](size_t, size_t begin, size_t end, int) {
        for (size_t task = begin; task < end; ++task) {
            const size_t i = order[task];
            const MeshPtr &mesh = model.meshes.at(int(i));
            MeshBake::Model::StageMs &ms = model.stageMs[int(i)];
            cancelPoint(cancel);
            Clock::time_point t = Clock::now();
            MeshBake::buildClusterDag(mesh, &dagStats[i]);
            ms.dag = msSince(t);
            cancelPoint(cancel);
            t = Clock::now();
            MeshBake::buildLevelTable(mesh);
            ms.levels = msSince(t);
            cancelPoint(cancel);
            t = Clock::now();
            MeshBake::buildCards(mesh, maxCards);
            ms.cards = msSince(t);
        }
    });
    // THE DEPTH CAP, named (the chain closed with the empty top over its remaining pool — a DAG all the same).
    for (size_t i = 0; i < meshes; ++i)
        if (dagStats[i].depthCapHit)
            irisLog(QStringLiteral("mesh bake: WARNING %1 mesh %2 \"%3\" (%4 clusters, %5 groups) met the cluster DAG's depth "
                                   "cap of %6 — closed with the empty top over the clusters it still held")
                        .arg(filePath).arg(i).arg(i < size_t(meshNames.size()) ? meshNames.at(int(i)) : QString())
                        .arg(dagStats[i].clusters).arg(dagStats[i].groups).arg(atomdag::kMaxLevels));
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
        parts << QStringLiteral("m%1 %2t levels %3 cards %4 dag %5")
                     .arg(i).arg(tris).arg(ms.levels, 0, 'f', 0).arg(ms.cards, 0, 'f', 0)
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
    QStringList meshNames;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) meshNames << QString::fromUtf8(scene->mMeshes[i]->mName.C_Str());
    bakeStages(model, filePath, meshNames, xf.maxCards, cancel);
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
