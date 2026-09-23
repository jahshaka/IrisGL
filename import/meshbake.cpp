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

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "assimp/Importer.hpp"
#include "assimp/scene.h"

// ATOM stage 1 (SPECS/NANITE_SPEC.md §7): the LOD chain is built HERE, once, at
// import. Vendored at thirdparty/meshoptimizer, pinned to the release tag v1.2;
// this and import/clusterlod.cpp (the one TU that compiles clusterlod.h's
// implementation, ATOM stage 2) are the only translation units that include it.
#include "meshoptimizer.h"
#include "thirdparty/meshoptimizer-clusterlod/clusterlod.h"
#include <chrono>

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
constexpr int kFormatVersion = 13;
constexpr quint32 kMagic = 0x4A4D424Bu;   // 'JMBK'

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
// Declared ahead of the builder that fills it (`clusterdag`, further down):
// the CONFIG RECORD is the builder's, and the reader refuses a blob written
// under any other.
namespace clusterdag { QVector<float> shippedConfigRecord(); }

void writeClusterDag(QDataStream &s, const MeshClusterDag &dag)
{
    s << qint32(dag.clusters.size());
    if (dag.isEmpty()) return;
    const QVector<float> record = clusterdag::shippedConfigRecord();
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
    const QVector<float> expected = clusterdag::shippedConfigRecord();
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

    // ATOM P2's trailing block (format v12) — the SIGNED DISTANCE FIELD. Three
    // zero dimensions for every mesh that gets none (skinned, not triangles, no
    // extent), which costs six bytes and an empty QByteArray.
    const MeshSdf &field = mesh->sdf;
    s << quint16(field.dim[0]) << quint16(field.dim[1]) << quint16(field.dim[2]);
    if (!field.isEmpty()) {
        writeVec3(s, field.origin);
        s << float(field.cell) << float(field.scale) << field.values;
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

    // ATOM P2's trailing block (format v12) — the SIGNED DISTANCE FIELD. Every
    // field is checked for the same reason the cards are: a consumer reads this
    // as geometry, and a grid whose byte count does not match its dimensions, or
    // whose cell is not a positive finite length, would be read as a surface that
    // is not there. Refuse the blob and parse instead.
    {
        quint16 dx = 0, dy = 0, dz = 0;
        s >> dx >> dy >> dz;
        if (s.status() != QDataStream::Ok) { *okOut = false; return MeshPtr(); }
        if (dx || dy || dz) {
            if (dx == 0 || dy == 0 || dz == 0 || dx > MeshSdf::kMaxDim || dy > MeshSdf::kMaxDim ||
                dz > MeshSdf::kMaxDim) { *okOut = false; return MeshPtr(); }
            MeshSdf field;
            field.dim[0] = dx; field.dim[1] = dy; field.dim[2] = dz;
            field.origin = readVec3(s);
            s >> field.cell >> field.scale >> field.values;
            if (s.status() != QDataStream::Ok || !(field.cell > 0.0f) ||
                !std::isfinite(field.cell) || !(field.scale > 0.0f) ||
                !std::isfinite(field.scale) ||
                !std::isfinite(field.origin.x()) || !std::isfinite(field.origin.y()) ||
                !std::isfinite(field.origin.z()) ||
                field.values.size() != field.cellCount()) { *okOut = false; return MeshPtr(); }
            mesh->sdf = field;
        }
    }

    // ATOM stage 2's trailing block (format v13) — the CLUSTER DAG.
    if (!readClusterDag(s, vertexCount, &mesh->clusterDag)) { *okOut = false; return MeshPtr(); }

    // THE PICKING MESH IS REBUILT, NOT STORED. It is positions + indices with
    // one cross product per triangle — cheaper to recompute than to read, and
    // recomputing is what makes it impossible for a bake to hand picking a
    // geometry the renderer does not have.
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


// ---- THE SURFACE: one sampler, one nearest-surface query -------------------
//
// ATOM-BAKE-1 (ATOM P1's AT-A5). Two products of the bake need to treat a mesh
// as a SURFACE rather than as a list of triangles — the card generator (which
// surfels it) and the LOD chain's honest error (which measures a distance
// between two of its levels) — and the surface sampler was written inside the
// card generator first. It lives HERE now, once, because a second stratified
// area sampler would be a second definition of "a point of this mesh".
namespace surface {

/// One sample of a triangle soup: a point on it, and the GEOMETRIC normal of
/// the triangle it came from (the shading normal can point somewhere the
/// surface does not face, and both consumers here raster or measure geometry).
struct Sample
{
    Vec3 pos;
    Vec3 nrm;
};

/// The van der Corput radical inverse — the low-discrepancy sequence that
/// replaces a random number generator here. `index` is the sample's own index,
/// so the k-th sample of a mesh is the k-th sample of that mesh forever: this
/// file contains no seed, which is stronger than "deterministic with a fixed
/// seed".
float radicalInverse(unsigned index, unsigned base)
{
    float result = 0.0f, f = 1.0f / float(base);
    while (index > 0) { result += f * float(index % base); index /= base; f /= float(base); }
    return result;
}

/// THE ONE STRATIFIED AREA SAMPLER.
///
/// What both consumers must cover is SURFACE, not vertices and not triangles: a
/// mesh with one enormous floor triangle and ten thousand tiny ones in a corner
/// would be sampled entirely in the corner by any per-triangle scheme. So the
/// triangle is chosen by a STRATIFIED walk of the cumulative-area array and the
/// barycentric coordinates come from a van der Corput pair.
///
/// Returns the soup's total area; fills `out` with AT MOST `count` samples (a
/// degenerate triangle contributes none — it has no facing, and a sample with no
/// normal is not a sample of a surface).
float sample(const float *positions, int posComps, const std::vector<unsigned> &indices,
             size_t count, std::vector<Sample> *out)
{
    out->clear();
    const size_t triCount = indices.size() / 3;
    if (triCount == 0 || count == 0 || !positions || posComps < 3) return 0.0f;

    const auto vertexAt = [&](unsigned i) {
        const float *v = positions + size_t(i) * size_t(posComps);
        return Vec3(v[0], v[1], v[2]);
    };

    std::vector<float> cumulative(triCount + 1, 0.0f);
    for (size_t t = 0; t < triCount; ++t) {
        const Vec3 a = vertexAt(indices[t * 3]);
        const Vec3 b = vertexAt(indices[t * 3 + 1]);
        const Vec3 c = vertexAt(indices[t * 3 + 2]);
        const float area = Vec3::crossProduct(b - a, c - a).length() * 0.5f;
        cumulative[t + 1] = cumulative[t] + (std::isfinite(area) ? area : 0.0f);
    }
    const float totalArea = cumulative.back();
    if (!(totalArea > 0.0f)) return 0.0f;

    out->reserve(count);
    for (size_t k = 0; k < count; ++k) {
        const float target = (float(k) + 0.5f) / float(count) * totalArea;
        const size_t t = size_t(std::upper_bound(cumulative.begin() + 1, cumulative.end(), target)
                                - cumulative.begin() - 1);
        if (t >= triCount) continue;
        const Vec3 a = vertexAt(indices[t * 3]);
        const Vec3 b = vertexAt(indices[t * 3 + 1]);
        const Vec3 c = vertexAt(indices[t * 3 + 2]);
        const Vec3 n = Vec3::crossProduct(b - a, c - a).normalized();
        if (n.lengthSquared() < 0.5f) continue;   // a degenerate triangle has no facing
        const float r1 = radicalInverse(unsigned(k) + 1u, 2u);
        const float r2 = radicalInverse(unsigned(k) + 1u, 3u);
        const float su = std::sqrt(r1);
        Sample s;
        s.pos = a * (1.0f - su) + b * (su * (1.0f - r2)) + c * (su * r2);
        s.nrm = n;
        out->push_back(s);
    }
    return totalArea;
}

/// The nearest point of one triangle to `p` (Ericson, Real-Time Collision
/// Detection §5.1.5 — the seven-region form, no square roots inside it).
Vec3 closestOnTriangle(const Vec3 &p, const Vec3 &a, const Vec3 &b, const Vec3 &c)
{
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = Vec3::dotProduct(ab, ap), d2 = Vec3::dotProduct(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    const Vec3 bp = p - b;
    const float d3 = Vec3::dotProduct(ab, bp), d4 = Vec3::dotProduct(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float denom = d1 - d3;
        return a + ab * (std::fabs(denom) > 0.0f ? d1 / denom : 0.0f);
    }

    const Vec3 cp = p - c;
    const float d5 = Vec3::dotProduct(ab, cp), d6 = Vec3::dotProduct(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float denom = d2 - d6;
        return a + ac * (std::fabs(denom) > 0.0f ? d2 / denom : 0.0f);
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float denom = (d4 - d3) + (d5 - d6);
        return b + (c - b) * (std::fabs(denom) > 0.0f ? (d4 - d3) / denom : 0.0f);
    }

    const float denom = va + vb + vc;
    if (!(std::fabs(denom) > 0.0f)) return a;
    return a + ab * (vb / denom) + ac * (vc / denom);
}

/// THE ANGLE THIS TRIANGLE SUBTENDS AT `q`, a point on it — the weight of the
/// ANGLE-WEIGHTED PSEUDONORMAL (Baerentzen & Aanaes, "Signed distance computation
/// using the angle weighted pseudonormal").
///
/// WHY A SIGNED DISTANCE FIELD CANNOT USE A FACE NORMAL. The sign of a point is
/// `dot(p - q, n)` where `n` is the surface normal AT `q`, and when `q` is on an
/// EDGE or a VERTEX there is no single face normal there — there are several, and
/// picking one of them is wrong for every point in the wedge between the others.
/// On a convex edge sharper than a right angle the error changes the SIGN: an
/// exterior point near a cone's tip, a pyramid's apex or a wedge's spine has its
/// nearest point on that feature, and the one face whose normal happens to be
/// picked can face away from it, so the point reads INSIDE. The angle-weighted sum
/// is the normal field whose sign is correct for every exterior point — that is
/// the theorem the paper proves, and it is why the weights are angles and not
/// areas.
///
///   * `q` interior to the face — one face, the whole 2*pi of directions: the face
///     normal, weight 2*pi.
///   * `q` on an edge — the two faces sharing it each subtend pi.
///   * `q` at a vertex — each incident face subtends its own INTERIOR ANGLE there,
///     which is the term that makes an asymmetric corner come out right.
float angleWeightAt(const Vec3 &q, const Vec3 &a, const Vec3 &b, const Vec3 &c, float eps)
{
    const float da = (q - a).length(), db = (q - b).length(), dc = (q - c).length();
    const auto interiorAngle = [](const Vec3 &at, const Vec3 &u, const Vec3 &v) {
        const Vec3 e1 = (u - at).normalized(), e2 = (v - at).normalized();
        const float d = std::clamp(Vec3::dotProduct(e1, e2), -1.0f, 1.0f);
        return std::acos(d);
    };
    if (da <= eps) return interiorAngle(a, b, c);
    if (db <= eps) return interiorAngle(b, a, c);
    if (dc <= eps) return interiorAngle(c, a, b);
    // On an edge? The distance from `q` to the line through that edge is zero, and
    // `q` is inside the segment (it is a closest point, so it cannot be outside).
    const auto onSegment = [&](const Vec3 &u, const Vec3 &v) {
        const Vec3 e = v - u;
        const float len2 = e.lengthSquared();
        if (!(len2 > 0.0f)) return false;
        const float s = std::clamp(Vec3::dotProduct(q - u, e) / len2, 0.0f, 1.0f);
        return ((u + e * s) - q).length() <= eps;
    };
    const float kPi = 3.14159265358979323846f;
    if (onSegment(a, b) || onSegment(b, c) || onSegment(c, a)) return kPi;
    return 2.0f * kPi;
}

/// A UNIFORM GRID OVER A TRIANGLE SOUP, and the EXACT nearest-surface query over
/// it. Never a brute-force loop: the bound measurement asks this thousands of
/// times per level and the SDF asks it per seeded cell.
///
/// Each triangle is filed in every cell its own AABB touches, so a query is a
/// shell walk outwards from the query point's cell that stops as soon as the
/// NEXT shell cannot hold anything closer than the best hit so far. That last
/// clause is what makes the answer EXACT and not approximate — it is the same
/// number the brute-force loop would return, found without visiting most of the
/// mesh. (A cell at Chebyshev ring distance s from the query point's own cell
/// cannot contain a point nearer than (s - 1) * cell, because the query point is
/// somewhere inside its own cell.)
class TriangleGrid
{
public:
    /// `res` is the ceiling on cells per axis; the cell is CUBIC (the largest
    /// axis divided by `res`) so the ring bound above is one length and not
    /// three.
    void build(const float *positions, int posComps, const std::vector<unsigned> &indices,
               const Vec3 &lo, const Vec3 &hi, int res)
    {
        mPositions = positions;
        mPosComps = posComps;
        mIndices = &indices;
        mLo = lo;
        mTriCount = indices.size() / 3;
        const Vec3 size = hi - lo;
        const float extent = std::max(std::max(size.x(), size.y()), size.z());
        mCell = extent > 0.0f ? extent / float(std::max(res, 1)) : 1.0f;
        for (int a = 0; a < 3; ++a) {
            const float s = a == 0 ? size.x() : (a == 1 ? size.y() : size.z());
            mDim[a] = std::clamp(int(std::floor(s / mCell)) + 1, 1, std::max(res, 1));
        }
        mCells.assign(size_t(mDim[0]) * size_t(mDim[1]) * size_t(mDim[2]), {});
        mStamp.assign(mTriCount, 0u);
        mGeneration = 0u;

        for (size_t t = 0; t < mTriCount; ++t) {
            Vec3 tlo = vertexAt((*mIndices)[t * 3]), thi = tlo;
            for (int k = 1; k < 3; ++k) {
                const Vec3 v = vertexAt((*mIndices)[t * 3 + size_t(k)]);
                tlo = Vec3(std::min(tlo.x(), v.x()), std::min(tlo.y(), v.y()), std::min(tlo.z(), v.z()));
                thi = Vec3(std::max(thi.x(), v.x()), std::max(thi.y(), v.y()), std::max(thi.z(), v.z()));
            }
            int c0[3], c1[3];
            cellOf(tlo, c0);
            cellOf(thi, c1);
            for (int z = c0[2]; z <= c1[2]; ++z)
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int x = c0[0]; x <= c1[0]; ++x)
                        mCells[index(x, y, z)].push_back(unsigned(t));
        }
    }

    bool empty() const { return mTriCount == 0; }

    /// The nearest point of the soup to `p`, and — when `normalOut` is asked for —
    /// the ANGLE-WEIGHTED PSEUDONORMAL there (see `angleWeightAt`: a face normal is
    /// the wrong answer on an edge or a vertex, and on a convex feature sharper
    /// than a right angle it gets the SIGN wrong). Returns the distance, or
    /// infinity for an empty soup.
    ///
    /// The tie set is collected as the walk goes and resolved at the end, because
    /// `best` only stops shrinking when the walk stops: a candidate is kept when it
    /// is within `eps` of the best SO FAR, and the final pass drops whatever the
    /// eventual best left behind. The scratch vector is a member so the hundreds of
    /// thousands of queries a bake makes allocate once.
    ///
    /// `triangleOut` receives the index (in the soup's own order) of the triangle
    /// the nearest point lies on — the cluster DAG's region assignment asks which
    /// simplified triangle a level-0 triangle now stands under.
    float closest(const Vec3 &p, Vec3 *pointOut = nullptr, Vec3 *normalOut = nullptr,
                  unsigned *triangleOut = nullptr) const
    {
        if (mTriCount == 0) return std::numeric_limits<float>::infinity();
        int base[3];
        cellOf(p, base);
        ++mGeneration;
        mTies.clear();
        float best = std::numeric_limits<float>::infinity();
        unsigned bestTri = 0u;
        Vec3 bestPoint, bestNormal(0, 1, 0);
        const int maxRing = std::max(std::max(mDim[0], mDim[1]), mDim[2]);
        for (int ring = 0; ring <= maxRing; ++ring) {
            // The stop rule, and the whole reason this is exact: nothing in ring
            // `ring` can beat `best` once (ring - 1) * cell already exceeds it.
            if (ring > 0 && float(ring - 1) * mCell > best) break;
            bool any = false;
            for (int z = base[2] - ring; z <= base[2] + ring; ++z) {
                if (z < 0 || z >= mDim[2]) continue;
                for (int y = base[1] - ring; y <= base[1] + ring; ++y) {
                    if (y < 0 || y >= mDim[1]) continue;
                    for (int x = base[0] - ring; x <= base[0] + ring; ++x) {
                        if (x < 0 || x >= mDim[0]) continue;
                        // Only the SHELL, not the solid block: the interior was
                        // scanned by the earlier rings.
                        if (ring > 0 && std::abs(x - base[0]) != ring &&
                            std::abs(y - base[1]) != ring && std::abs(z - base[2]) != ring)
                            continue;
                        any = true;
                        for (unsigned t : mCells[index(x, y, z)]) {
                            if (mStamp[t] == mGeneration) continue;   // filed in several cells
                            mStamp[t] = mGeneration;
                            const Vec3 a = vertexAt((*mIndices)[size_t(t) * 3]);
                            const Vec3 b = vertexAt((*mIndices)[size_t(t) * 3 + 1]);
                            const Vec3 c = vertexAt((*mIndices)[size_t(t) * 3 + 2]);
                            const Vec3 q = closestOnTriangle(p, a, b, c);
                            const float d = (q - p).length();
                            const float eps = tieEps(d);
                            if (normalOut) {
                                // THE TIE SET IS THE NEAREST FEATURE'S FACES AND
                                // NOTHING ELSE, and bounding it is not tidiness —
                                // it is the difference between 0.4 s and 9 s of
                                // bake on `endlessplane.obj`. A cell high above a
                                // large flat mesh is nearly equidistant from
                                // THOUSANDS of its triangles, so "within eps of the
                                // best so far" collected the mesh; an angle weight
                                // (an acos and two normalises) per collected
                                // triangle per cell then dominated everything.
                                // A strictly nearer hit RESTARTS the set, since the
                                // feature has changed; `kMaxTies` is generous for
                                // the real cases — the faces around one vertex.
                                if (d < best - eps) mTies.clear();
                                if (d <= best + eps && mTies.size() < kMaxTies)
                                    mTies.push_back({ q, a, b, c, d });
                            }
                            if (d < best) { best = d; bestPoint = q; bestTri = t; }
                        }
                    }
                }
            }
            (void)any;
        }
        if (normalOut) {
            // The angle-weighted sum over every triangle that really is nearest.
            const float eps = tieEps(best);
            Vec3 sum(0, 0, 0);
            for (const Tie &tie : mTies) {
                if (tie.d > best + eps) continue;
                const Vec3 n = Vec3::crossProduct(tie.b - tie.a, tie.c - tie.a).normalized();
                if (n.lengthSquared() < 0.5f) continue;
                sum = sum + n * angleWeightAt(tie.q, tie.a, tie.b, tie.c, eps);
            }
            const float len = sum.length();
            if (len > 0.0f) bestNormal = sum / len;
            *normalOut = bestNormal;
        }
        if (pointOut) *pointOut = bestPoint;
        if (triangleOut) *triangleOut = bestTri;
        return best;
    }

    float cell() const { return mCell; }

    /// The tolerance that decides "the same nearest feature". Two faces sharing an
    /// edge give analytically equal distances to a point on it and differ only by
    /// round-off, so the test has to be a tolerance; it is tied to the CELL (the
    /// coordinate scale) with a term in the distance itself for far queries.
    /// Genuinely distinct surfaces at the same distance are a medial-axis point,
    /// where the sign is ill-defined however it is computed.
    float tieEps(float d) const { return std::max(mCell * 1e-4f, std::fabs(d) * 1e-4f); }

private:
    Vec3 vertexAt(unsigned i) const
    {
        const float *v = mPositions + size_t(i) * size_t(mPosComps);
        return Vec3(v[0], v[1], v[2]);
    }
    void cellOf(const Vec3 &p, int out[3]) const
    {
        const float c[3] = { p.x() - mLo.x(), p.y() - mLo.y(), p.z() - mLo.z() };
        for (int a = 0; a < 3; ++a)
            out[a] = std::clamp(int(std::floor(c[a] / mCell)), 0, mDim[a] - 1);
    }
    size_t index(int x, int y, int z) const
    {
        return (size_t(z) * size_t(mDim[1]) + size_t(y)) * size_t(mDim[0]) + size_t(x);
    }

    const float *mPositions = nullptr;
    int mPosComps = 3;
    const std::vector<unsigned> *mIndices = nullptr;
    size_t mTriCount = 0;
    Vec3 mLo;
    float mCell = 1.0f;
    int mDim[3] = { 1, 1, 1 };
    std::vector<std::vector<unsigned>> mCells;
    mutable std::vector<unsigned> mStamp;
    mutable unsigned mGeneration = 0;
    /// One candidate nearest feature: where on the triangle, the triangle, and how
    /// far. Kept only while a caller asks for a normal.
    struct Tie { Vec3 q, a, b, c; float d = 0.0f; };
    /// The faces meeting at one vertex, with room to spare. A nearest FEATURE is a
    /// face (1), an edge (2) or a vertex (its incident faces); anything beyond this
    /// is a near-equidistant crowd that contributes nothing to a pseudonormal.
    static constexpr size_t kMaxTies = 32;
    mutable std::vector<Tie> mTies;
};

}   // namespace surface

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
// it pays for the sample count alone, which is why it travels with N.
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

/// THE MEASURED TWO-SIDED DISTANCE between two index lists over ONE vertex
/// buffer (every level of a chain shares the vertices, ATOM rule 1), before the
/// margin. `gridA` must be the grid of `a`, `gridB` of `b`.
///
/// TWO SAMPLE SETS, AND THE SECOND ONE IS THE MAXIMUM — it was added after the
/// acceptance suite caught its absence and then CORRECTED after the lane's audit
/// read what it was actually measuring.
///
///   * AREA SAMPLES, both ways, cover the surfaces evenly and are what an average
///     deviation needs.
///   * AND THE BASE VERTICES THE LEVEL NO LONGER HAS. A Hausdorff distance between
///     two piecewise-linear surfaces is attained at a vertex or on an edge, never
///     in the middle of a facet, and `meshopt_simplify` REMOVES vertices without
///     ever moving one (its output is an index list over the ORIGINAL vertex
///     buffer — ATOM rule 1, one vertex buffer and N index buffers). So the whole
///     of the worst case sits on the vertices level 0 has and level k does not,
///     and those are the only points this walk needs: a vertex level k still USES
///     is a vertex of one of its own triangles, whose distance to level k is
///     exactly zero.
///
///     THE FIRST CUT WALKED BOTH DIRECTIONS AND CAPPED THE WALK, and both were
///     wrong. Level k's own vertices against level 0 measured 0 every single time
///     (they are level-0 vertices, on level-0 triangles); and the walk that DID
///     matter — level 0's vertices against level k — was strided past a 200 000
///     probe cap, so a million-vertex mesh probed one vertex in five and the
///     margin cannot cover a SKIPPED vertex, only an unsampled facet. The set
///     difference is the fix in both directions at once: no dead loop, no stride,
///     and it is SMALLER than either walk was (a halving step removes about half
///     the vertices, and only those are queried).
///
/// `includeRemovedVertices` exists for the acceptance suite, which measures
/// AREA-ONLY on purpose so that it exercises the sampling-gap margin instead of
/// reproducing the exact term the bake already took a maximum over
/// (`MeshBake::checkLodBounds`).
float twoSidedDistance(const float *positions, int posComps,
                       const std::vector<unsigned> &a, const surface::TriangleGrid &gridA,
                       const std::vector<unsigned> &b, const surface::TriangleGrid &gridB,
                       size_t samples, bool includeRemovedVertices = true)
{
    float worst = 0.0f;
    std::vector<surface::Sample> pts;
    const auto areaOneWay = [&](const std::vector<unsigned> &from,
                                const surface::TriangleGrid &against) {
        if (surface::sample(positions, posComps, from, samples, &pts) > 0.0f)
            for (const surface::Sample &s : pts) {
                const float d = against.closest(s.pos);
                if (std::isfinite(d)) worst = std::max(worst, d);
            }
    };
    areaOneWay(a, gridB);
    areaOneWay(b, gridA);
    if (!includeRemovedVertices) return worst;

    // THE VERTICES `b` (level 0) HAS AND `a` (the level) DOES NOT, against the
    // level's own surface. Sorted-unique both sides, then one set_difference —
    // deterministic, allocation-bounded by the vertex count, no stride.
    std::vector<unsigned> base(b.begin(), b.end());
    std::sort(base.begin(), base.end());
    base.erase(std::unique(base.begin(), base.end()), base.end());
    std::vector<unsigned> kept(a.begin(), a.end());
    std::sort(kept.begin(), kept.end());
    kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
    std::vector<unsigned> removed;
    removed.reserve(base.size());
    std::set_difference(base.begin(), base.end(), kept.begin(), kept.end(),
                        std::back_inserter(removed));
    for (unsigned v : removed) {
        const float *p = positions + size_t(v) * size_t(posComps);
        const float d = gridA.closest(Vec3(p[0], p[1], p[2]));
        if (std::isfinite(d)) worst = std::max(worst, d);
    }
    return worst;
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

void build(const MeshPtr &mesh)
{
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
        const size_t n = attrCount
            ? meshopt_simplifyWithAttributes(out.data(), prev.data(), prev.size(),
                                             positions, nv, posStride,
                                             attribs.data(), sizeof(float) * attrCount,
                                             weights.data(), attrCount,
                                             nullptr, target, FLT_MAX, options, &stepError)
            : meshopt_simplify(out.data(), prev.data(), prev.size(),
                               positions, nv, posStride,
                               target, FLT_MAX, options, &stepError);
        if (n < 3 || n % 3 != 0) break;
        // Topology can stop the simplifier short. A level that did not shed at
        // least 15% costs a buffer and a switch for nothing.
        if (float(n) > float(prev.size()) * kAcceptRatio) break;

        const float error = std::max(accumulated, stepError);
        if (extent > 0.0f && error > extent * kMaxRelError) break;

        out.resize(n);

        // THE MEASUREMENT (AT-A5). Level `out` against level 0, both ways.
        surface::TriangleGrid levelGrid;
        levelGrid.build(positions, posComps, out, lo, hi, boundGridRes(out.size() / 3));
        const float measured =
            twoSidedDistance(positions, posComps, out, levelGrid, base, baseGrid, boundSamples) *
            kBoundMargin;
        // MONOTONE NON-DECREASING BY CONSTRUCTION, and that is a requirement and
        // not a tidy-up: `lodLevelForWorldError` walks the array and stops at the
        // FIRST level it cannot afford, which is only the right answer for a
        // sorted array. A sampled maximum is not guaranteed to rise with the
        // level (a coarser level can happen to land closer at the points these
        // samples fall on), so the running max is taken — which over-states, i.e.
        // errs towards a FINER level than needed, which is the safe direction.
        // THE TWO TERMS, SEPARATELY, behind a run-wide latch (the idiom every
        // measurable rule in this tree carries). It is how the margin and the vertex
        // walk are re-checked without a build, and what it measured is why the
        // vertex walk exists: on `endlessplane.obj` the removed vertices raise the
        // level-2 bound from 4.589 to 60.033 — THIRTEEN TIMES — because a flat mesh
        // deviates only where its RIM was cut, which no area sampler will land on.
        // On smooth meshes they add 1.008x to 1.046x, i.e. almost nothing, which is
        // the other half of the honest statement.
        if (std::getenv("JAH_BAKE_BOUND_TERMS")) {
            surface::TriangleGrid g2;
            g2.build(positions, posComps, out, lo, hi, boundGridRes(out.size() / 3));
            const float areaOnly = twoSidedDistance(positions, posComps, out, g2, base, baseGrid,
                                                    boundSamples, false) * kBoundMargin;
            irisLog(QStringLiteral("bound terms: level %1  area-only %2  with-removed-verts %3  "
                                   "verts add %4x")
                        .arg(mesh->lodIndices.size() + 1)
                        .arg(double(areaOnly), 0, 'f', 6).arg(double(measured), 0, 'f', 6)
                        .arg(areaOnly > 0.0f ? double(measured) / double(areaOnly) : 0.0, 0, 'f', 3));
        }
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

// ---------------------------------------------------------------------------
// ATOM P2 / SUB-S5-SDF — THE PER-MESH SIGNED DISTANCE FIELD.
//
// A third product of the one bake step, beside the chain and the cards
// (document/assets/mesh.h MeshSdf carries what it is and why the sign of an open
// mesh is a pseudo-normal). This is HOW.
//
//   1. THE GRID IS MESH-RELATIVE AND CUBIC, and its cell is the coarser of two
//      things: the largest axis divided by the resolution ceiling, and FOUR
//      TIMES LEVEL 1'S MEASURED BOUND. The second term is the whole reason the
//      field is built after the chain: a field finer than the surface is honest
//      is recording detail the bake has already declined to promise. Four,
//      because a distance field is read by trilinear interpolation and a feature
//      needs about two cells on each side of it to survive one.
//
//   2. A JUMP FLOOD FOR THE DISTANCE (the design's word, and the right one for a
//      DISTANCE: the cost is then independent of the triangle count). Seeded from
//      the TRIANGLES, because a query per cell would BE the answer and make the
//      flood decoration; log2(dim) passes then halve the step and let each cell
//      adopt the nearest seed any neighbour knows about. NO NORMAL IS CARRIED.
//
//   3. THE BAND IS EXACT. Every cell within `kExactBand` cells of the surface
//      re-asks the grid for its own nearest point and the ANGLE-WEIGHTED
//      PSEUDONORMAL there, so the ZERO CROSSING — the only part a consumer reads
//      for a surface distance, and the only part a suite can check — is the
//      brute-force answer in magnitude AND in sign.
//
//   4. AND THE SIGN IS FLOODED OUTWARDS, which is the one sound way to sign a far
//      cell. A sign changes only by crossing the surface, the surface is entirely
//      inside the band, and the band is thicker than one cell — so a breadth-first
//      sweep from the band carries the sign to every cell and cannot skip the
//      crossing. Carrying a NORMAL through the distance flood instead is what the
//      first cut did, and it is unsound: a far cell inherits the normal of its
//      SEED's nearest point, not of its own. MEASURED on `cone.obj` — eight
//      exterior cells read NEGATIVE because a cell outside the base rim inherited
//      the normal of a cell under the base. Asking EVERY cell exactly fixes the
//      sign and costs 8.8 s on `endlessplane.obj` (a far cell's shell walk is
//      stopped only by its own distance, so it is unbounded by construction);
//      flooding the sign is exact where it matters and one sweep everywhere else.
namespace sdf {

/// THE KNOBS, in one place, with the reason each exists. BAKE INPUTS: editing any
/// of them re-bakes every library by itself (meshbake.cpp is in the producer
/// hash).
constexpr int   kMinRes         = 16;    ///< a field below this cannot hold a shape at all; a thin axis is padded up to it.
constexpr int   kMaxRes         = MeshSdf::kMaxDim;   ///< and the format's ceiling, stated once beside the struct.
constexpr int   kPadCells       = 2;     ///< cells of OUTSIDE band around the mesh's box, so the field has an exterior to be positive in.
constexpr float kCellPerBound   = 4.0f;  ///< the cell is at least this many times level 1's measured bound (see 1 above).
/// ...AND AT LEAST THE MESH'S OWN TRIANGLE SCALE, which is what a mesh with NO
/// CHAIN has instead of a bound. MEASURED, because the first cut did not have
/// this term and it was the whole bake's cost: a mesh the simplifier could not
/// reduce has no `lodBounds`, fell through to the resolution ceiling, and a
/// TWELVE-TRIANGLE CUBE was given a 64^3 field — 262 144 cells and 41 million
/// jump-flood neighbour tests to describe six planes, 1.45 SECONDS per primitive
/// (cone 1.83 s, wedge 1.73 s, cylinder 1.66 s), which is what blew
/// `meshbake.roundtrip`'s 120 s timeout. The RMS triangle edge
/// `sqrt(2 * area / triangles)` is the honest statement of what resolution that
/// mesh's geometry actually carries, and at 1.0 of it the same cube gets 16^3 and
/// costs single-digit milliseconds.
constexpr float kCellPerEdge    = 1.0f;
constexpr float kRangeCells     = 8.0f;  ///< `scale`: the distance |value| == 127 stands for, in cells. Beyond it the field saturates and only the sign is meaningful.
/// THE BAND `checkSdfAgainstSurface` JUDGES THE ZERO CROSSING IN, in cells. Every
/// cell is exact now (see 2 above), so this is no longer a policy of the generator
/// — it is the width of the region the suite asserts to within one cell, which is
/// the region a consumer reads for a surface distance rather than for occupancy.
constexpr float kExactBand      = 2.5f;


void build(const MeshPtr &mesh)
{
    if (mesh.isNull()) return;
    mesh->sdf = MeshSdf();
    if (!mesh->getSkeleton().isNull()) return;         // the surface moves: a baked field would be a lie
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

    Vec3 lo(positions[0], positions[1], positions[2]), hi = lo;
    for (size_t v = 0; v < nv; ++v) {
        const float *p = positions + v * size_t(posComps);
        lo = Vec3(std::min(lo.x(), p[0]), std::min(lo.y(), p[1]), std::min(lo.z(), p[2]));
        hi = Vec3(std::max(hi.x(), p[0]), std::max(hi.y(), p[1]), std::max(hi.z(), p[2]));
    }
    const Vec3 size = hi - lo;
    const float extent = std::max(std::max(size.x(), size.y()), size.z());
    if (!(extent > 0.0f)) return;

    // (1) THE RESOLUTION, in three steps, and every step has one job.
    //
    //   a. HOW FINE THE GEOMETRY IS HONEST TO. Two terms, the coarser winning:
    //      four times level 1's measured bound (a field finer than the surface is
    //      honest records detail the bake has declined to promise), and the mesh's
    //      own RMS triangle edge (what a mesh with NO chain has instead — see
    //      `kCellPerEdge`, which is a measurement, not a guess).
    //   b. THE CEILING. The largest axis plus its pad must fit in `kMaxRes`.
    //   c. AND THE FIT. Once the dimensions are clamped, the cubic cell is
    //      RE-DERIVED from them so the mesh's box exactly fills the interior on
    //      its longest axis and the grid is CENTRED on the box. Without this step
    //      a coarse honest cell met the `kMinRes` floor and produced a grid many
    //      times the size of the object, sitting off to one side of it.
    float honestCell = 0.0f;
    if (!mesh->lodBounds.isEmpty())
        honestCell = mesh->lodBounds.first() * kCellPerBound;
    {
        const auto vertexAt = [&](unsigned i) {
            const float *v = positions + size_t(i) * size_t(posComps);
            return Vec3(v[0], v[1], v[2]);
        };
        double area = 0.0;
        const size_t triCount = indices.size() / 3;
        for (size_t tri = 0; tri < triCount; ++tri) {
            const Vec3 a = vertexAt(indices[tri * 3]);
            const Vec3 b = vertexAt(indices[tri * 3 + 1]);
            const Vec3 c = vertexAt(indices[tri * 3 + 2]);
            const float f = Vec3::crossProduct(b - a, c - a).length() * 0.5f;
            if (std::isfinite(f)) area += double(f);
        }
        if (triCount > 0 && area > 0.0)
            honestCell = std::max(honestCell,
                                  kCellPerEdge * float(std::sqrt(2.0 * area / double(triCount))));
    }
    const int span = std::max(kMaxRes - 1 - 2 * kPadCells, 1);
    float cell = std::max(extent / float(span), honestCell);
    if (!(cell > 0.0f)) return;

    quint16 dim[3] = { 0, 0, 0 };
    for (int a = 0; a < 3; ++a) {
        const float s = a == 0 ? size.x() : (a == 1 ? size.y() : size.z());
        const int need = int(std::floor(s / cell)) + 1 + 2 * kPadCells;
        dim[a] = quint16(std::clamp(need, kMinRes, kMaxRes));
    }
    // (c) the fit: the cubic cell the clamped dimensions can actually carry.
    cell = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const float s = a == 0 ? size.x() : (a == 1 ? size.y() : size.z());
        const int interior = std::max(int(dim[a]) - 1 - 2 * kPadCells, 1);
        cell = std::max(cell, s / float(interior));
    }
    if (!(cell > 0.0f)) return;
    // ...and the grid centred on the mesh's box.
    Vec3 origin;
    {
        float o[3];
        for (int a = 0; a < 3; ++a) {
            const float s = a == 0 ? size.x() : (a == 1 ? size.y() : size.z());
            const float l = a == 0 ? lo.x() : (a == 1 ? lo.y() : lo.z());
            o[a] = l - (float(int(dim[a]) - 1) * cell - s) * 0.5f;
        }
        origin = Vec3(o[0], o[1], o[2]);
    }
    const size_t count = size_t(dim[0]) * size_t(dim[1]) * size_t(dim[2]);

    surface::TriangleGrid grid;
    grid.build(positions, posComps, indices, lo, hi,
               lodchain::boundGridRes(indices.size() / 3));

    const auto centreOf = [&](size_t x, size_t y, size_t z) {
        return origin + Vec3(float(x) * cell, float(y) * cell, float(z) * cell);
    };
    const auto at = [&](size_t x, size_t y, size_t z) {
        return (z * size_t(dim[1]) + y) * size_t(dim[0]) + x;
    };

    // (2) THE DISTANCE, FLOODED. Seeded from the TRIANGLES (a query per cell would
    // BE the answer and make the flood decoration), then log2(dim) jump-flood
    // passes. This gives every cell a distance without a query — and NOTHING ELSE:
    // no normal is carried, because a normal cannot be.
    const size_t triCount = indices.size() / 3;
    std::vector<Vec3> seed(count);
    std::vector<float> dist(count, std::numeric_limits<float>::infinity());
    std::vector<char> has(count, 0);
    {
        const auto vertexAt = [&](unsigned i) {
            const float *v = positions + size_t(i) * size_t(posComps);
            return Vec3(v[0], v[1], v[2]);
        };
        for (size_t tri = 0; tri < triCount; ++tri) {
            const Vec3 a = vertexAt(indices[tri * 3]);
            const Vec3 b = vertexAt(indices[tri * 3 + 1]);
            const Vec3 c = vertexAt(indices[tri * 3 + 2]);
            Vec3 tlo(std::min(std::min(a.x(), b.x()), c.x()),
                     std::min(std::min(a.y(), b.y()), c.y()),
                     std::min(std::min(a.z(), b.z()), c.z()));
            Vec3 thi(std::max(std::max(a.x(), b.x()), c.x()),
                     std::max(std::max(a.y(), b.y()), c.y()),
                     std::max(std::max(a.z(), b.z()), c.z()));
            int c0[3], c1[3];
            for (int ax = 0; ax < 3; ++ax) {
                const float o = ax == 0 ? origin.x() : (ax == 1 ? origin.y() : origin.z());
                const float l = ax == 0 ? tlo.x() : (ax == 1 ? tlo.y() : tlo.z());
                const float h = ax == 0 ? thi.x() : (ax == 1 ? thi.y() : thi.z());
                c0[ax] = std::clamp(int(std::floor((l - o) / cell)) - 1, 0, int(dim[ax]) - 1);
                c1[ax] = std::clamp(int(std::floor((h - o) / cell)) + 1, 0, int(dim[ax]) - 1);
            }
            for (int z = c0[2]; z <= c1[2]; ++z)
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int x = c0[0]; x <= c1[0]; ++x) {
                        const size_t i = at(size_t(x), size_t(y), size_t(z));
                        const Vec3 p = centreOf(size_t(x), size_t(y), size_t(z));
                        const Vec3 q = surface::closestOnTriangle(p, a, b, c);
                        const float d = (q - p).length();
                        if (d < dist[i]) { dist[i] = d; seed[i] = q; has[i] = 1; }
                    }
        }
    }
    const int maxDim = std::max(std::max(int(dim[0]), int(dim[1])), int(dim[2]));
    for (int step = maxDim / 2; step >= 1; step /= 2) {
        std::vector<Vec3> nextSeed = seed;
        std::vector<float> nextDist = dist;
        std::vector<char> nextHas = has;
        for (int z = 0; z < int(dim[2]); ++z)
            for (int y = 0; y < int(dim[1]); ++y)
                for (int x = 0; x < int(dim[0]); ++x) {
                    const size_t self = at(size_t(x), size_t(y), size_t(z));
                    const Vec3 p = centreOf(size_t(x), size_t(y), size_t(z));
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                if (!dx && !dy && !dz) continue;
                                const int nx = x + dx * step, ny = y + dy * step, nz = z + dz * step;
                                if (nx < 0 || ny < 0 || nz < 0 || nx >= int(dim[0]) ||
                                    ny >= int(dim[1]) || nz >= int(dim[2])) continue;
                                const size_t other = at(size_t(nx), size_t(ny), size_t(nz));
                                if (!has[other]) continue;
                                const float d = (seed[other] - p).length();
                                if (d < nextDist[self]) {
                                    nextDist[self] = d; nextSeed[self] = seed[other]; nextHas[self] = 1;
                                }
                            }
                }
        seed.swap(nextSeed); dist.swap(nextDist); has.swap(nextHas);
    }

    // (3) THE BAND IS EXACT, AND THE BAND IS WHERE THE ZERO SET IS. Every cell
    // within `kExactBand` cells re-asks the grid for its own nearest point and the
    // ANGLE-WEIGHTED PSEUDONORMAL there, so both its magnitude and its SIGN are the
    // brute-force answer (`surface::angleWeightAt` says why a face normal is not,
    // and how it flips the sign at a cone tip or a wedge spine).
    const float scale = cell * kRangeCells;
    const float band = cell * kExactBand;
    std::vector<float> signedValue(count, 0.0f);
    std::vector<char> known(count, 0);
    for (size_t i = 0; i < count; ++i) {
        if (!has[i] || dist[i] > band) continue;
        const size_t x = i % size_t(dim[0]);
        const size_t y = (i / size_t(dim[0])) % size_t(dim[1]);
        const size_t z = i / (size_t(dim[0]) * size_t(dim[1]));
        const Vec3 p = centreOf(x, y, z);
        Vec3 nearest, normal(0, 1, 0);
        const float d = grid.closest(p, &nearest, &normal);
        if (!std::isfinite(d)) continue;
        const float side = Vec3::dotProduct(p - nearest, normal);
        signedValue[i] = side < 0.0f ? -d : d;
        known[i] = 1;
    }

    // (4) AND THE SIGN IS FLOODED OUTWARDS, WHICH IS SOUND AND A NORMAL IS NOT.
    //
    // THE ARGUMENT, because this is the step that replaced a defect: a signed
    // distance changes sign only by crossing the surface, and the surface lies
    // ENTIRELY inside the band computed above — which is at least two cells thick on
    // each side. A 6-connected step moves ONE cell, so no path from an outside cell
    // to an inside cell can get past the band without entering it. Therefore a
    // breadth-first sweep from the band's cells carries the sign correctly to every
    // cell, with no query and no possibility of inheriting the wrong surface.
    //
    // WHAT THIS REPLACED, measured: carrying the angle-weighted NORMAL through the
    // flood and signing with it gave `cone.obj` EIGHT exterior cells reading
    // negative (`checkSdfExteriorSign`), because a cell outside the base rim
    // inherited the normal of a cell under the base. Asking every cell exactly
    // instead fixed the sign and cost 8.8 SECONDS on `endlessplane.obj` — a far
    // cell's shell walk is unbounded by construction, since its own best distance is
    // what stops it. Flooding the SIGN is exact where it matters, sound everywhere,
    // and costs one sweep.
    {
        std::vector<size_t> frontier;
        frontier.reserve(count);
        for (size_t i = 0; i < count; ++i) if (known[i]) frontier.push_back(i);
        const int dimX = int(dim[0]), dimY = int(dim[1]), dimZ = int(dim[2]);
        while (!frontier.empty()) {
            std::vector<size_t> next;
            for (size_t i : frontier) {
                const int x = int(i % size_t(dimX));
                const int y = int((i / size_t(dimX)) % size_t(dimY));
                const int z = int(i / (size_t(dimX) * size_t(dimY)));
                const float sign = signedValue[i] < 0.0f ? -1.0f : 1.0f;
                const int off[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
                for (const auto &o : off) {
                    const int nx = x + o[0], ny = y + o[1], nz = z + o[2];
                    if (nx < 0 || ny < 0 || nz < 0 || nx >= dimX || ny >= dimY || nz >= dimZ)
                        continue;
                    const size_t j = at(size_t(nx), size_t(ny), size_t(nz));
                    if (known[j]) continue;
                    // The magnitude is the flood's (it saturates out here anyway);
                    // the SIGN is this neighbour's, by the argument above.
                    const float magnitude = has[j] ? dist[j] : scale;
                    signedValue[j] = sign * magnitude;
                    known[j] = 1;
                    next.push_back(j);
                }
            }
            frontier.swap(next);
        }
    }

    QByteArray values(int(count), 0);
    char *out = values.data();
    for (size_t i = 0; i < count; ++i) {
        // A cell the sweep never reached has no surface anywhere near it; OUTSIDE is
        // the honest answer and it saturates.
        const float v = known[i] ? signedValue[i] : scale;
        const float clamped = std::clamp(v / scale, -1.0f, 1.0f);
        out[int(i)] = static_cast<char>(
            static_cast<signed char>(std::lround(clamped * 127.0f)));
    }

    mesh->sdf.dim[0] = dim[0];
    mesh->sdf.dim[1] = dim[1];
    mesh->sdf.dim[2] = dim[2];
    mesh->sdf.origin = origin;
    mesh->sdf.cell = cell;
    mesh->sdf.scale = scale;
    mesh->sdf.values = values;
}

}   // namespace sdf

// ---- ATOM stage 2: THE CLUSTER DAG (lane ATOM-CLUSTER-1) ------------------
//
// SPECS/atom/B2_CLUSTER_DAG_DESIGN.md §1, and the verified facts about the
// vendored header in SPECS/NANITE_SPEC.md §2c. A SECOND product of the same
// bake, beside the chain and not instead of it: the chain is what the
// voxeliser, the cards, the far BLAS and the cull's per-object path read; the
// DAG is what stage 3's GPU cut will read. Nothing in the product draws it yet
// (the lead's shaping decision in the design's preamble).
//
// WHAT IS BUILT. `clodBuild` (thirdparty/meshoptimizer-clusterlod/clusterlod.h,
// compiled once in import/clusterlod.cpp) on LEVEL 0: 128-triangle leaf
// clusters, groups of about sixteen neighbours merged, each group simplified to
// half and re-split, until one cluster is left. The header hands every group to
// the callback with the clusters that are its MEMBERS; a cluster's `refined` is
// the group whose simplification produced it. Stored meshlet-local
// (`clodLocalIndices`): per cluster a slice of mesh vertices and three 8-bit
// indices per triangle into it.
//
// WHAT IS MEASURED, AND WHY clusterlod's OWN ERROR IS NOT THE ONE THE RULE READS.
// `clodGroup::simplified.error` is `meshopt_simplify`'s `result_error` pushed
// through the header's monotone merge — the same quadric ESTIMATE that ATOM P1's
// AT-A5 measured the chain's levels against and found is not a bound (the reasons
// are listed above `lodchain::twoSidedDistance`). So each group's error is
// MEASURED exactly the way a chain level's is: the sampled two-sided distance
// (area samples both ways, plus the removed level-0 vertices against the
// simplified surface, exactly) between the group's SIMPLIFIED geometry — the
// clusters whose `refined` is this group — and the LEVEL-0 triangles it stands
// for, times the same sampling-gap margin, floored at the same arithmetic noise.
// The header's number is stored beside it as a diagnostic.
//
// "THE LEVEL-0 TRIANGLES IT STANDS FOR" IS PROVENANCE, and it is computed, not
// assumed. A leaf cluster stands for its own triangles. A group stands for the
// union of its members. A group's simplified output is re-split into clusters,
// and each of those stands for the part of that union that is NEAREST it — every
// level-0 triangle of the union is given to the output cluster whose surface is
// closest to its centroid. Group boundaries are LOCKED through the whole build
// (the header's `lockBoundary`), so an output's boundary is its group's boundary
// and the nearest-surface split is the honest partition. It is a partition by
// construction (each triangle goes to exactly one output), which is what makes
// atom.cluster_cut's "exactly one cut covers every level-0 triangle" checkable.
//
// TWO MONOTONICITIES ARE ENFORCED, and both are what make the per-cluster rule
// CRACK-FREE rather than tidy. The rule draws a cluster iff its group is not
// affordable and its `refined` group is; for that to select exactly one cut, a
// group that is affordable must have every CHILD group (the `refined` of each of
// its members) affordable too. Affordable is `error < allowed(distance to the
// group's sphere)`, so it needs (1) error(parent) >= error(child) — a sampled
// maximum is not guaranteed to rise, so the bake raises it and COUNTS the fix —
// and (2) the parent's sphere CONTAINS the child's, so the parent is never
// further from the eye than the child (the header merges spheres
// conservatively; the bake asserts it in float and grows the radius where
// rounding says otherwise, and counts that too).
namespace clusterdag {

/// THE LEAF: at most 128 triangles and 128 vertices per cluster —
/// `clodDefaultConfig(128)`, Nanite's own leaf, and the chain's triangle floor
/// (`lodchain::kMinTriangles`) is the same number for the same reason.
constexpr int kMaxTriangles = 128;
/// A mesh with fewer than two leaves' worth of triangles is ONE cluster, i.e.
/// level 0 itself: a DAG would add a buffer and a rule for nothing.
constexpr int kMinTrianglesForDag = 2 * kMaxTriangles;
/// Area samples per group per direction: two per simplified triangle (S -> level
/// 0) and one per level-0 facet that lost a corner (level 0 -> S), at least 512
/// and at most the chain's own per-level budget — so a group is sampled at least
/// as densely as the chain samples a whole level (the dragon's chain takes 4096
/// samples over 68,220 triangles), and the exact term, the removed vertices, is
/// walked in full either way.
constexpr int kGroupSamplesMin = 512;

using Variant = MeshBake::ClusterDagVariant;

/// THE MEASUREMENT'S PICK (tests/atom/cluster_config_measure.cpp `configs`; the
/// table is in ~/Developer/spikes/atom-cluster-1/config-measure.txt): PERMISSIVE,
/// NOTHING PROTECTED — the seams are paid for in the error, exactly stage 1's
/// measured choice for the chain (rule 4 above `lodchain`), and the opposite of
/// clusterlod.h's default pairing (permissive + a UV protect mask). Judged by the
/// cut's triangle count at seven equal measured bounds (0.05 % to 5 % of the
/// extent) over the eleven shipped meshes that get a DAG: the mean of cut /
/// level-0 is 0.627 here, 0.647 with the UV seams protected (the header's
/// pairing, and the same with normals too), 0.665 strict, 0.669 regularised —
/// and 0.658 for the chain at the same bounds. The difference is the dragon,
/// whose UV seams are a third of its vertices: at 1 % of its extent protecting
/// them draws 21,754 triangles and charging them 10,588. Changing this re-bakes
/// every library twice over: meshbake.cpp is in the producer hash, and the
/// blob's config record no longer matches.
constexpr Variant kShipped = Variant::PermissiveCharged;

Variant resolve(Variant v) { return v == Variant::Shipped ? kShipped : v; }

clodConfig configFor(Variant v)
{
    clodConfig c = clodDefaultConfig(size_t(kMaxTriangles));
    switch (resolve(v)) {
    case Variant::RegularizedProtectUv:
        c.simplify_regularize = true;
        break;
    case Variant::Strict:
        c.simplify_permissive = false;
        c.simplify_fallback_permissive = true;
        break;
    default:
        break;
    }
    return c;
}

/// Which attributes are PROTECTED (their discontinuities may not be collapsed
/// across). Attribute K of the interleaved buffer is bit K: the normal's three
/// components first when the mesh has normals, then the UV's two.
unsigned protectMaskFor(Variant v, bool normals, bool uvs)
{
    const unsigned nBits = normals ? 0x7u : 0u;
    const unsigned uvShift = normals ? 3u : 0u;
    const unsigned uvBits = uvs ? (0x3u << uvShift) : 0u;
    switch (resolve(v)) {
    case Variant::DefaultProtectUv:
    case Variant::RegularizedProtectUv:
        return uvBits;
    case Variant::DefaultProtectAll:
        return nBits | uvBits;
    default:
        return 0u;
    }
}

/// THE CONFIG RECORD the blob carries: every field of `clodConfig` and the
/// protect policy, in declaration order, as floats (every value is a small
/// integer, a flag or a float, so the representation is exact). A blob whose
/// record is not this build's is REFUSED, so a config change invalidates bakes
/// on its own — not only through the producer hash.
QVector<float> configRecord(Variant v)
{
    const clodConfig c = configFor(v);
    const Variant r = resolve(v);
    const float protect = r == Variant::DefaultProtectAll ? 2.0f
                        : (r == Variant::DefaultProtectUv || r == Variant::RegularizedProtectUv) ? 1.0f
                        : 0.0f;
    return QVector<float>{
        float(c.max_vertices), float(c.min_triangles), float(c.max_triangles),
        float(c.partition_spatial), float(c.partition_sort), float(c.partition_size),
        float(c.cluster_spatial), c.cluster_fill_weight, c.cluster_split_factor,
        c.simplify_ratio, c.simplify_threshold,
        c.simplify_error_merge_previous, c.simplify_error_merge_additive,
        c.simplify_error_factor_sloppy, c.simplify_error_edge_limit,
        float(c.simplify_permissive), float(c.simplify_fallback_permissive),
        float(c.simplify_fallback_sloppy), float(c.simplify_regularize),
        float(c.optimize_bounds), float(c.optimize_clusters), float(c.optimize_clusters_level),
        protect, float(kMaxTriangles), lodchain::kNormalWeight, lodchain::kUvWeight,
        lodchain::kBoundMargin, lodchain::kBoundFloorRel };
}

Vec3 vertexOf(const float *positions, int posComps, unsigned v)
{
    const float *p = positions + size_t(v) * size_t(posComps);
    return Vec3(p[0], p[1], p[2]);
}

void build(const MeshPtr &mesh, MeshBake::ClusterDagStats *stats, Variant variant)
{
    if (mesh.isNull()) return;
    mesh->clusterDag = MeshClusterDag();
    if (stats) {
        const bool want = stats->wantRegions;
        *stats = MeshBake::ClusterDagStats();
        stats->wantRegions = want;
    }
    if (!mesh->getSkeleton().isNull()) return;     // static meshes only, as the chain
    if (mesh->primitiveMode != PrimitiveMode::Triangles) return;

    int posComps = 3; size_t nv = 0;
    const float *positions = lodchain::attribData(mesh, VertexAttribUsage::Position, &posComps, &nv);
    if (!positions || posComps < 3 || nv < 3) return;
    const IndexBufferPtr ib = mesh->getIndexBuffer();
    if (ib.isNull() || !ib->data || ib->dataSize <= 0) return;
    std::vector<unsigned> base(reinterpret_cast<const unsigned *>(ib->data),
                               reinterpret_cast<const unsigned *>(ib->data) +
                                   size_t(ib->dataSize) / sizeof(unsigned));
    if (base.size() < 3 || base.size() % 3 != 0) return;
    for (unsigned i : base) if (size_t(i) >= nv) return;
    const size_t baseTris = base.size() / 3;
    if (baseTris < size_t(kMinTrianglesForDag)) return;

    // THE ATTRIBUTE METRIC IS THE CHAIN'S (rule 3 above `lodchain`): normals at
    // meshoptimizer's reference weight, UVs at a weight DERIVED from the mesh's
    // own extent and UV range. The interleaving [nx ny nz][u v] is also what the
    // protect mask's bit numbering assumes (`protectMaskFor`).
    int nrmComps = 3; size_t nrmCount = 0;
    const float *normals = lodchain::attribData(mesh, VertexAttribUsage::Normal, &nrmComps, &nrmCount);
    if (normals && nrmCount < nv) normals = nullptr;
    int uvComps = 2; size_t uvCount = 0;
    const float *uvs = lodchain::attribData(mesh, VertexAttribUsage::TexCoord0, &uvComps, &uvCount);
    if (uvs && (uvCount < nv || uvComps < 2)) uvs = nullptr;
    const size_t posStride = sizeof(float) * size_t(posComps);
    const float extent = meshopt_simplifyScale(positions, nv, posStride);

    const size_t attrCount = (normals ? 3u : 0u) + (uvs ? 2u : 0u);
    std::vector<float> attribs, weights;
    if (attrCount) {
        attribs.assign(nv * attrCount, 0.0f);
        weights.assign(attrCount, lodchain::kNormalWeight);
        float umin = FLT_MAX, umax = -FLT_MAX, vmin = FLT_MAX, vmax = -FLT_MAX;
        for (size_t v = 0; v < nv; ++v) {
            float *dst = &attribs[v * attrCount];
            if (normals) { for (int c = 0; c < 3; ++c) dst[c] = normals[v * size_t(nrmComps) + size_t(c)]; dst += 3; }
            if (uvs) {
                const float u = uvs[v * size_t(uvComps)], w = uvs[v * size_t(uvComps) + 1];
                dst[0] = u; dst[1] = w;
                umin = std::min(umin, u); umax = std::max(umax, u);
                vmin = std::min(vmin, w); vmax = std::max(vmax, w);
            }
        }
        if (uvs) {
            float range = std::max(umax - umin, vmax - vmin);
            if (!(range > 1e-6f)) range = 1.0f;
            const float uvWeight = lodchain::kUvWeight * (extent > 0.0f ? extent : 1.0f) / range;
            weights[attrCount - 2] = uvWeight;
            weights[attrCount - 1] = uvWeight;
        }
    }

    clodMesh cm = {};
    cm.indices = base.data();
    cm.index_count = base.size();
    cm.vertex_count = nv;
    cm.vertex_positions = positions;
    cm.vertex_positions_stride = posStride;
    cm.vertex_attributes = attrCount ? attribs.data() : nullptr;
    cm.vertex_attributes_stride = sizeof(float) * attrCount;
    cm.vertex_lock = nullptr;
    cm.attribute_weights = attrCount ? weights.data() : nullptr;
    cm.attribute_count = attrCount;
    cm.attribute_protect_mask = protectMaskFor(variant, normals != nullptr, uvs != nullptr);

    // ---- the build ------------------------------------------------------
    struct RawCluster
    {
        std::vector<unsigned> indices;
        int group = -1;
        int refined = -1;
        clodBounds bounds = {};
    };
    std::vector<RawCluster> raw;
    std::vector<clodGroup> groups;
    const auto t0 = std::chrono::steady_clock::now();
    clodBuild(configFor(variant), cm,
              [&](clodGroup group, const clodCluster *clusters, size_t count) -> int {
                  const int id = int(groups.size());
                  groups.push_back(group);
                  for (size_t i = 0; i < count; ++i) {
                      RawCluster rc;
                      rc.indices.assign(clusters[i].indices, clusters[i].indices + clusters[i].index_count);
                      rc.group = id;
                      rc.refined = clusters[i].refined;
                      rc.bounds = clusters[i].bounds;
                      raw.push_back(std::move(rc));
                  }
                  return id;
              });
    const auto t1 = std::chrono::steady_clock::now();
    if (raw.empty() || groups.empty()) return;

    // ---- provenance + the measured error ---------------------------------
    std::vector<std::vector<int>> members(groups.size()), outputs(groups.size());
    for (size_t c = 0; c < raw.size(); ++c) {
        if (raw[c].group < 0 || raw[c].group >= int(groups.size())) return;   // the header broke its contract
        if (raw[c].refined >= int(groups.size())) return;
        members[size_t(raw[c].group)].push_back(int(c));
        if (raw[c].refined >= 0) outputs[size_t(raw[c].refined)].push_back(int(c));
    }

    // THE PROVENANCE IS OF VERTICES. `meshopt_simplify` never moves a vertex and
    // never makes one, so at every depth a level-0 vertex is either KEPT (some
    // cluster still uses it) or REMOVED, and a removed vertex stands under the
    // cluster whose surface is nearest it. So each cluster carries the level-0
    // vertices it stands for: its OWN vertices, plus every vertex its group
    // removed — now or at any depth below — that is nearest its surface. A
    // group's region is the union of its members'; the measurement below walks
    // it, and hands every removed vertex on to exactly one output cluster.
    //
    // A KEPT vertex on a group border belongs to both sides and costs nothing
    // (it is ON both surfaces); that is what keeps the measurement free of the
    // STAIRCASE a triangle provenance has (measured and replaced: a level-0
    // triangle given to the output nearest its centroid leaves level-0 triangles
    // on the far side of every simplified border edge, which at the next depth
    // read as deviation — the 20k sphere's depth-1 groups measured 0.011 against
    // clusterlod's 0.0006, and the DAG could not coarsen below a quarter of the
    // triangles until 1.4 % of the extent where the chain was at an eighth by
    // 0.5 %; and a triangle given to the owner of a removed corner let a vertex
    // on a thin feature name a cluster across the gap).
    //
    // `regionOwn` is the same walk with every vertex given to ONE cluster (a
    // kept vertex to the first output using it): a PARTITION of level 0 at every
    // depth, reported to the suites (a level-0 triangle belongs where its first
    // corner does) so "exactly one cut covers every triangle" can be counted.
    std::vector<std::vector<unsigned>> regionAll(raw.size()), regionOwn(raw.size());
    std::vector<unsigned> vertexStamp(nv, 0u), ownStamp(nv, 0u);
    unsigned stamp = 1;
    const auto ownVertices = [&](size_t c, std::vector<unsigned> &out) {
        ++stamp;
        for (unsigned v : raw[c].indices)
            if (vertexStamp[v] != stamp) { vertexStamp[v] = stamp; out.push_back(v); }
    };
    for (size_t c = 0; c < raw.size(); ++c) {
        if (raw[c].refined != -1) continue;
        ownVertices(c, regionAll[c]);
        for (unsigned v : raw[c].indices)
            if (!ownStamp[v]) { ownStamp[v] = 1; regionOwn[c].push_back(v); }
    }
    // Level-0 triangles by their first corner, for the area term and the stats.
    std::vector<std::vector<unsigned>> trisByAnchor(nv);
    for (size_t t = 0; t < baseTris; ++t) trisByAnchor[base[t * 3]].push_back(unsigned(t));

    // The WHOLE level-0 surface, once: the simplified geometry is measured
    // against it (the "did it add surface" direction needs no provenance).
    Vec3 mlo = vertexOf(positions, posComps, base[0]), mhi = mlo;
    for (unsigned v : base) {
        const Vec3 p = vertexOf(positions, posComps, v);
        mlo = Vec3(std::min(mlo.x(), p.x()), std::min(mlo.y(), p.y()), std::min(mlo.z(), p.z()));
        mhi = Vec3(std::max(mhi.x(), p.x()), std::max(mhi.y(), p.y()), std::max(mhi.z(), p.z()));
    }
    surface::TriangleGrid baseGrid;
    baseGrid.build(positions, posComps, base, mlo, mhi, lodchain::boundGridRes(baseTris));

    const size_t samplesCap = baseTris > size_t(lodchain::kBoundBigTriangles)
                                  ? size_t(lodchain::kBoundSamplesBig) : size_t(lodchain::kBoundSamples);
    const float floorLen = extent > 0.0f ? extent * lodchain::kBoundFloorRel : 0.0f;

    std::vector<float> measured(groups.size(), FLT_MAX);
    int belowEstimate = 0;
    std::vector<int> keptOwner(nv, -1), removedOwner(nv, -1);
    std::vector<unsigned> inRegion(nv, 0u), keptStamp(nv, 0u);
    std::vector<surface::Sample> pts;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (groups[g].simplified.error == FLT_MAX || outputs[g].empty()) continue;   // terminal
        const unsigned here = ++stamp;
        // S: the group's simplified output, and who keeps which vertex.
        std::vector<unsigned> simplifiedIdx, ownerOfTri;
        for (int o : outputs[g]) {
            const std::vector<unsigned> &idx = raw[size_t(o)].indices;
            simplifiedIdx.insert(simplifiedIdx.end(), idx.begin(), idx.end());
            ownerOfTri.insert(ownerOfTri.end(), idx.size() / 3, unsigned(o));
            for (unsigned v : idx) if (keptStamp[v] != here) { keptStamp[v] = here; keptOwner[v] = o; }
        }
        // R: the level-0 vertices the members stand for.
        std::vector<unsigned> regionV;
        for (int m : members[g])
            for (unsigned v : regionAll[size_t(m)])
                if (inRegion[v] != here) { inRegion[v] = here; regionV.push_back(v); }
        if (regionV.empty() || simplifiedIdx.empty()) continue;

        // The grid over S spans the group, not the mesh: a group is a small patch
        // of a large mesh, and a grid over the mesh's box would file the patch
        // into a handful of cells and walk all of it per query.
        Vec3 glo = vertexOf(positions, posComps, simplifiedIdx[0]), ghi = glo;
        for (const std::vector<unsigned> *list : { &regionV, &simplifiedIdx })
            for (unsigned v : *list) {
                const Vec3 p = vertexOf(positions, posComps, v);
                glo = Vec3(std::min(glo.x(), p.x()), std::min(glo.y(), p.y()), std::min(glo.z(), p.z()));
                ghi = Vec3(std::max(ghi.x(), p.x()), std::max(ghi.y(), p.y()), std::max(ghi.z(), p.z()));
            }
        surface::TriangleGrid gridS;
        gridS.build(positions, posComps, simplifiedIdx, glo, ghi,
                    lodchain::boundGridRes(simplifiedIdx.size() / 3));

        // THE MEASUREMENT, in the chain's three terms (`lodchain::twoSidedDistance`
        // states why each exists), each on the side that needs no provenance or
        // the provenance that has no staircase:
        //   1. area samples of S against the WHOLE level-0 surface;
        //   2. every REMOVED vertex of R against S, exactly — where the maximum of
        //      the level-0 -> S direction lives (a kept vertex is on S: zero);
        //   3. area samples of the level-0 facets that LOST ALL THREE corners here
        //      or below against S — the facets between the removed vertices.
        //      (Not every facet with a removed corner: a facet with a KEPT corner
        //      can sit on a border, half under a neighbour's surface, and the
        //      first cut of this term read that as 0.014 of deviation on the 20k
        //      sphere where its vertices measured 0.001 — the staircase again.)
        float worst = 0.0f;
        const size_t sTris = simplifiedIdx.size() / 3;
        if (surface::sample(positions, posComps, simplifiedIdx,
                            std::clamp(sTris * 2u, size_t(kGroupSamplesMin), samplesCap), &pts) > 0.0f)
            for (const surface::Sample &sp : pts) {
                const float d = baseGrid.closest(sp.pos);
                if (std::isfinite(d)) worst = std::max(worst, d);
            }
        const float termS = worst;
        for (unsigned v : regionV) {
            removedOwner[v] = -1;
            if (keptStamp[v] == here) continue;                 // KEPT: on S, distance 0
            unsigned tri = 0u;
            const float d = gridS.closest(vertexOf(positions, posComps, v), nullptr, nullptr, &tri);
            if (std::isfinite(d)) worst = std::max(worst, d);
            removedOwner[v] = int(ownerOfTri[std::min<size_t>(tri, ownerOfTri.size() - 1)]);
        }
        const float termV = worst;
        std::vector<unsigned> lostIdx;
        for (unsigned v : regionV)
            for (unsigned t : trisByAnchor[v]) {
                const unsigned b = base[t * 3 + 1], c = base[t * 3 + 2];
                if (inRegion[b] != here || inRegion[c] != here) continue;
                if (keptStamp[v] == here || keptStamp[b] == here || keptStamp[c] == here) continue;
                lostIdx.insert(lostIdx.end(), { v, b, c });
            }
        if (!lostIdx.empty() &&
            surface::sample(positions, posComps, lostIdx,
                            std::clamp(lostIdx.size() / 3, size_t(kGroupSamplesMin), samplesCap), &pts) > 0.0f)
            for (const surface::Sample &sp : pts) {
                const float d = gridS.closest(sp.pos);
                if (std::isfinite(d)) worst = std::max(worst, d);
            }
        measured[g] = std::max(worst * lodchain::kBoundMargin, floorLen);
        if (measured[g] < groups[g].simplified.error) ++belowEstimate;
        if (std::getenv("JAH_BAKE_DAG_TERMS"))
            irisLog(QStringLiteral("dag terms: group %1 depth %2  R %3 verts  S %4 tris  S->L0 %5  "
                                   "removed verts %6  lost facets %7  estimate %8")
                        .arg(g).arg(groups[g].depth).arg(regionV.size()).arg(sTris)
                        .arg(double(termS), 0, 'g', 4).arg(double(termV), 0, 'g', 4)
                        .arg(double(worst), 0, 'g', 4).arg(double(groups[g].simplified.error), 0, 'g', 4));

        // HAND ON: every output carries its own vertices plus the removed ones
        // nearest it; the partition gives each kept vertex to its first user.
        for (int o : outputs[g]) ownVertices(size_t(o), regionAll[size_t(o)]);
        for (unsigned v : regionV)
            if (removedOwner[v] >= 0) regionAll[size_t(removedOwner[v])].push_back(v);
        for (int m : members[g])
            for (unsigned v : regionOwn[size_t(m)]) {
                const int o = keptStamp[v] == here ? keptOwner[v] : removedOwner[v];
                if (o >= 0) regionOwn[size_t(o)].push_back(v);
            }
    }

    // ---- the two monotonicities (children always have LOWER ids) ---------
    MeshClusterDag dag;
    dag.groups.resize(int(groups.size()));
    int monotoneFixes = 0, sphereFixes = 0, terminal = 0, maxDepth = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
        MeshClusterDag::Group &out = dag.groups[int(g)];
        const clodGroup &src = groups[g];
        out.depth = src.depth;
        maxDepth = std::max(maxDepth, src.depth);
        for (int k = 0; k < 3; ++k) out.centre[k] = src.simplified.center[k];
        out.radius = src.simplified.radius;
        out.estimate = src.simplified.error;
        out.error = measured[g];
        if (out.error == FLT_MAX) ++terminal;
        for (int m : members[g]) {
            const int child = raw[size_t(m)].refined;
            if (child < 0) continue;
            const MeshClusterDag::Group &cg = dag.groups[child];
            if (out.error != FLT_MAX && out.error < cg.error) { out.error = cg.error; ++monotoneFixes; }
            const float dx = out.centre[0] - cg.centre[0], dy = out.centre[1] - cg.centre[1],
                        dz = out.centre[2] - cg.centre[2];
            const float need = std::sqrt(dx * dx + dy * dy + dz * dz) + cg.radius;
            if (out.radius < need) {
                // Grown with a relative hair of slack so the float evaluation at
                // the rule can never see the child poke out again.
                out.radius = need * (1.0f + 1e-6f);
                ++sphereFixes;
            }
        }
    }

    // ---- meshlet-local storage -------------------------------------------
    dag.clusters.resize(int(raw.size()));
    std::vector<unsigned> localVerts;
    std::vector<unsigned char> localTris;
    for (size_t c = 0; c < raw.size(); ++c) {
        const std::vector<unsigned> &idx = raw[c].indices;
        localVerts.assign(idx.size(), 0u);
        localTris.assign(idx.size(), 0u);
        const size_t unique = clodLocalIndices(localVerts.data(), localTris.data(), idx.data(), idx.size());
        MeshClusterDag::Cluster &out = dag.clusters[int(c)];
        out.vertexOffset = quint32(dag.vertices.size());
        out.triangleOffset = quint32(dag.triangles.size() / 3);
        out.vertexCount = quint16(unique);
        out.triangleCount = quint16(idx.size() / 3);
        out.group = raw[c].group;
        out.refined = raw[c].refined;
        for (int k = 0; k < 3; ++k) out.centre[k] = raw[c].bounds.center[k];
        out.radius = raw[c].bounds.radius;
        for (size_t i = 0; i < unique; ++i) dag.vertices.append(quint32(localVerts[i]));
        dag.triangles.append(reinterpret_cast<const char *>(localTris.data()), int(idx.size()));
    }
    mesh->clusterDag = dag;
    const auto t2 = std::chrono::steady_clock::now();

    if (stats) {
        stats->clusters = int(raw.size());
        stats->groups = int(groups.size());
        stats->depth = maxDepth + 1;
        stats->terminalGroups = terminal;
        stats->monotoneFixes = monotoneFixes;
        stats->sphereFixes = sphereFixes;
        stats->measuredBelowEstimate = belowEstimate;
        stats->buildMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        stats->measureMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
        if (stats->wantRegions) {
            stats->clusterRegions.resize(int(raw.size()));
            for (size_t c = 0; c < raw.size(); ++c) {
                QVector<quint32> &out = stats->clusterRegions[int(c)];
                for (unsigned v : regionOwn[c])
                    for (unsigned t : trisByAnchor[v]) out.append(quint32(t));
            }
        }
    }
}

QVector<float> shippedConfigRecord() { return configRecord(kShipped); }

}   // namespace clusterdag


}   // namespace

void MeshBake::buildLodChain(const MeshPtr &mesh) { lodchain::build(mesh); }

void MeshBake::buildClusterDag(const MeshPtr &mesh, ClusterDagStats *stats, ClusterDagVariant variant)
{
    clusterdag::build(mesh, stats, variant);
}

MeshBake::ClusterDagVariant MeshBake::shippedClusterDagVariant() { return clusterdag::kShipped; }

const char *MeshBake::clusterDagVariantName(ClusterDagVariant variant)
{
    switch (clusterdag::resolve(variant)) {
    case ClusterDagVariant::DefaultProtectUv:     return "default+protect-uv";
    case ClusterDagVariant::DefaultProtectAll:    return "default+protect-nrm+uv";
    case ClusterDagVariant::PermissiveCharged:    return "permissive-charged";
    case ClusterDagVariant::RegularizedProtectUv: return "regularized+protect-uv";
    case ClusterDagVariant::Strict:               return "strict(fallback-permissive)";
    default:                                      return "?";
    }
}

void MeshBake::buildCards(const MeshPtr &mesh, int maxCards) { cards::build(mesh, maxCards); }

void MeshBake::buildSdf(const MeshPtr &mesh) { sdf::build(mesh); }

bool MeshBake::checkLodBounds(const MeshPtr &mesh, int densityMultiple, double *worstRatioOut)
{
    if (worstRatioOut) *worstRatioOut = 0.0;
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
        const float measured = lodchain::twoSidedDistance(positions, posComps, level, levelGrid,
                                                          base, baseGrid, dense);
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

bool MeshBake::checkSdfAgainstSurface(const MeshPtr &mesh, double *worstCellsOut, int *probedOut)
{
    if (worstCellsOut) *worstCellsOut = 0.0;
    if (probedOut) *probedOut = 0;
    if (mesh.isNull() || mesh->sdf.isEmpty()) return false;
    int posComps = 3; size_t nv = 0;
    const float *positions =
        lodchain::attribData(mesh, VertexAttribUsage::Position, &posComps, &nv);
    if (!positions || posComps < 3 || nv < 3) return false;
    const IndexBufferPtr ib = mesh->getIndexBuffer();
    if (ib.isNull() || !ib->data || ib->dataSize <= 0) return false;
    std::vector<unsigned> indices(reinterpret_cast<const unsigned *>(ib->data),
                                  reinterpret_cast<const unsigned *>(ib->data) +
                                      size_t(ib->dataSize) / sizeof(unsigned));
    if (indices.size() < 3 || indices.size() % 3 != 0) return false;

    Vec3 lo(positions[0], positions[1], positions[2]), hi = lo;
    for (size_t v = 0; v < nv; ++v) {
        const float *p = positions + v * size_t(posComps);
        lo = Vec3(std::min(lo.x(), p[0]), std::min(lo.y(), p[1]), std::min(lo.z(), p[2]));
        hi = Vec3(std::max(hi.x(), p[0]), std::max(hi.y(), p[1]), std::max(hi.z(), p[2]));
    }
    surface::TriangleGrid grid;
    grid.build(positions, posComps, indices, lo, hi,
               lodchain::boundGridRes(indices.size() / 3));

    const MeshSdf &f = mesh->sdf;
    double worst = 0.0;
    int probed = 0;
    const float band = f.cell * sdf::kExactBand;
    for (int z = 0; z < int(f.dim[2]); ++z)
        for (int y = 0; y < int(f.dim[1]); ++y)
            for (int x = 0; x < int(f.dim[0]); ++x) {
                const float stored = f.distanceAt(x, y, z);
                if (std::fabs(stored) > band) continue;      // outside the exact band
                const Vec3 p = f.origin + Vec3(float(x) * f.cell, float(y) * f.cell,
                                               float(z) * f.cell);
                const float exact = grid.closest(p);
                if (!std::isfinite(exact)) continue;
                ++probed;
                worst = std::max(worst, double(std::fabs(std::fabs(stored) - exact)) /
                                            double(f.cell));
            }
    if (worstCellsOut) *worstCellsOut = worst;
    if (probedOut) *probedOut = probed;
    return worst <= 1.0;
}

bool MeshBake::checkSdfExteriorSign(const MeshPtr &mesh, int *probedOut, int *wrongOut)
{
    if (probedOut) *probedOut = 0;
    if (wrongOut) *wrongOut = 0;
    if (mesh.isNull() || mesh->sdf.isEmpty()) return false;
    int posComps = 3; size_t nv = 0;
    const float *positions =
        lodchain::attribData(mesh, VertexAttribUsage::Position, &posComps, &nv);
    if (!positions || posComps < 3 || nv < 3) return false;
    const IndexBufferPtr ib = mesh->getIndexBuffer();
    if (ib.isNull() || !ib->data || ib->dataSize <= 0) return false;
    std::vector<unsigned> indices(reinterpret_cast<const unsigned *>(ib->data),
                                  reinterpret_cast<const unsigned *>(ib->data) +
                                      size_t(ib->dataSize) / sizeof(unsigned));
    if (indices.size() < 3 || indices.size() % 3 != 0) return false;
    const auto vertexAt = [&](unsigned i) {
        const float *v = positions + size_t(i) * size_t(posComps);
        return Vec3(v[0], v[1], v[2]);
    };

    // RAY PARITY ALONG +X, and the ONLY thing it is asked is "is this point
    // outside": an even crossing count is outside for a closed mesh. A ray that
    // grazes an edge or a vertex is UNANSWERABLE rather than wrong, so such a cell
    // is skipped (reported through `probedOut`, which the suite asserts is large).
    const MeshSdf &f = mesh->sdf;
    int probed = 0, wrong = 0;
    const size_t triCount = indices.size() / 3;
    for (int z = 0; z < int(f.dim[2]); ++z)
        for (int y = 0; y < int(f.dim[1]); ++y)
            for (int x = 0; x < int(f.dim[0]); ++x) {
                const float stored = f.distanceAt(x, y, z);
                // Within a cell of the surface the true sign is ambiguous at this
                // resolution; beyond saturation the magnitude says nothing but the
                // SIGN still must be right, so those cells are kept.
                if (std::fabs(stored) <= f.cell) continue;
                const Vec3 p = f.origin + Vec3(float(x) * f.cell, float(y) * f.cell,
                                               float(z) * f.cell);
                int crossings = 0;
                bool ambiguous = false;
                for (size_t tri = 0; tri < triCount && !ambiguous; ++tri) {
                    const Vec3 a = vertexAt(indices[tri * 3]);
                    const Vec3 b = vertexAt(indices[tri * 3 + 1]);
                    const Vec3 c = vertexAt(indices[tri * 3 + 2]);
                    // Moeller-Trumbore against the +X ray, with the degenerate and
                    // near-edge cases declared ambiguous instead of guessed.
                    const Vec3 e1 = b - a, e2 = c - a;
                    const Vec3 dir(1.0f, 0.0f, 0.0f);
                    const Vec3 pv = Vec3::crossProduct(dir, e2);
                    const float det = Vec3::dotProduct(e1, pv);
                    if (std::fabs(det) < 1e-12f) continue;          // parallel: no crossing
                    const float inv = 1.0f / det;
                    const Vec3 tv = p - a;
                    const float u = Vec3::dotProduct(tv, pv) * inv;
                    const Vec3 qv = Vec3::crossProduct(tv, e1);
                    const float v = Vec3::dotProduct(dir, qv) * inv;
                    const float w = 1.0f - u - v;
                    if (u < -1e-5f || v < -1e-5f || w < -1e-5f) continue;   // outside the face
                    if (u < 1e-4f || v < 1e-4f || w < 1e-4f) { ambiguous = true; break; }
                    const float tHit = Vec3::dotProduct(e2, qv) * inv;
                    if (tHit < 1e-5f) continue;                     // behind the origin
                    ++crossings;
                }
                if (ambiguous) continue;
                const bool outside = (crossings % 2) == 0;
                if (!outside) continue;                             // only the exterior is judged
                ++probed;
                if (!(stored > 0.0f)) {
                    ++wrong;
                    if (wrong <= 8)
                        irisLog(QStringLiteral("sdf sign: exterior cell (%1,%2,%3) reads %4 "
                                               "(%5 cells from the surface, band %6)")
                                    .arg(x).arg(y).arg(z)
                                    .arg(double(stored), 0, 'f', 6)
                                    .arg(double(std::fabs(stored) / f.cell), 0, 'f', 2)
                                    .arg(double(sdf::kExactBand), 0, 'f', 1));
                }
            }
    if (probedOut) *probedOut = probed;
    if (wrongOut) *wrongOut = wrong;
    return wrong == 0;
}

int MeshBake::cardCaptureResolution() { return cards::kCaptureResolution; }

MeshBake::Model MeshBake::buildFromScene(const SceneSource &source, const QString &filePath,
                                         const QString &fingerprint, const QString &extractDir,
                                         const ImportTransform &xf)
{
    return buildFromScene(source.scene(), filePath, fingerprint, extractDir, xf);
}

MeshBake::Model MeshBake::buildFromScene(const aiScene *scene, const QString &filePath,
                                         const QString &fingerprint, const QString &extractDir,
                                         const ImportTransform &xf)
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
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh *m = scene->mMeshes[i];
        // THE TUNING SWITCHES (import/importsettings.h §4.3). They act on what
        // is BUILT, never on the parse: `skeleton:false` drops the skeleton AND
        // the bone index/weight arrays, so a rigged file bakes as static
        // geometry; the same two lines are what MeshNode::loadAsSceneFragment
        // does, because the bake and the parse fallback have to agree node for
        // node (tests/meshbake compares the two trees).
        auto mesh = MeshPtr(new Mesh(const_cast<aiMesh *>(m), xf.skeleton));
        if (m->HasBones() && xf.skeleton) mesh->setSkeleton(Mesh::extractSkeleton(m, scene));
        // ATOM stage 1: the LOD chain is a product of the bake, built here and
        // nowhere else. The fallback parse path (a library with no bake yet)
        // gets no chain — which is the same "no LOD" behaviour the tree has
        // today, and one more reason a bake is worth having.
        MeshBake::buildLodChain(mesh);
        // SURFACE-CACHE phase 1: the card list, built from the chain (a card
        // names the level its texel picks), so the order of these two lines is
        // load-bearing.
        MeshBake::buildCards(mesh, xf.maxCards);
        // ATOM P2 / SUB-S5-SDF: the signed distance field, also built from the
        // chain (its cell may not be finer than level 1's measured bound), so it
        // comes third and the order of all three lines is load-bearing.
        MeshBake::buildSdf(mesh);
        // ATOM stage 2: the cluster DAG. Independent of the three above (it
        // simplifies level 0 itself), so its place in the order is free.
        MeshBake::buildClusterDag(mesh);
        model.meshes.append(mesh);

        const unsigned aiMatIndex = m->mMaterialIndex;
        auto known = materialIndexMap.constFind(aiMatIndex);
        if (known != materialIndexMap.constEnd()) {
            materialFor[int(i)] = known.value();
            continue;
        }
        MeshMaterialData data;
        if (xf.materials && aiMatIndex < scene->mNumMaterials)
            MaterialHelper::extractMaterialData(scene, scene->mMaterials[aiMatIndex],
                                                dir, data, extractDir, filePath);
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
                                        const QString &extractDir, const ImportTransform &xf)
{
    Assimp::Importer importer;
    // The lazy re-bake's parse, through the choke point with the ASSET's
    // import transform — the bake IS the transformed geometry, so a re-bake
    // that dropped the transform would quietly un-scale a library
    // (import/scenesource.h).
    const aiScene *scene = readSceneFile(importer, filePath, iris::ImportFlags::Canonical, xf);
    if (!scene) {
        irisLog("mesh bake: assimp could not read " + filePath);
        return Model();
    }
    return buildFromScene(scene, filePath, fingerprint, extractDir, xf);
}

// ---- serialize / deserialize ----------------------------------------------

QByteArray MeshBake::serialize(const Model &model)
{
    QByteArray blob;
    QDataStream s(&blob, QIODevice::WriteOnly);
    configure(s);
    s << quint32(kMagic) << qint32(kFormatVersion) << model.fingerprint;
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
