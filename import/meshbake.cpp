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
#include <cstring>
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
// this is the ONLY translation unit in the tree that includes it.
#include "meshoptimizer.h"

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
// import (levelCount, then per level its index list and its geometric error as
// a length in mesh units). A v6 blob has no chain at all, so replaying one
// would silently ship a library whose models never drop a triangle, with no way
// for any fingerprint to notice; and the block is a layout change besides.
constexpr int kFormatVersion = 7;
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
    const int levels = std::min(mesh->lodIndices.size(), mesh->lodErrors.size());
    s << qint32(levels);
    for (int i = 0; i < levels; ++i) {
        const QVector<quint32> &idx = mesh->lodIndices.at(i);
        s << qint32(idx.size()) << float(mesh->lodErrors.at(i));
        s << QByteArray(reinterpret_cast<const char *>(idx.constData()),
                        idx.size() * int(sizeof(quint32)));
    }
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
    for (qint32 i = 0; i < levels; ++i) {
        qint32 indexCount = 0; float error = 0.0f;
        s >> indexCount >> error;
        QByteArray levelBytes;
        s >> levelBytes;
        if (s.status() != QDataStream::Ok || indexCount < 3 || indexCount % 3 != 0 ||
            levelBytes.size() != indexCount * int(sizeof(quint32))) { *okOut = false; return MeshPtr(); }
        QVector<quint32> idx(indexCount);
        std::memcpy(idx.data(), levelBytes.constData(), size_t(levelBytes.size()));
        // A level that names a vertex the mesh does not have would draw
        // garbage the moment the camera backs off — refuse the blob instead.
        for (quint32 v : idx) if (int(v) >= vertexCount) { *okOut = false; return MeshPtr(); }
        mesh->lodIndices.append(idx);
        mesh->lodErrors.append(error);
    }

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

QString MeshBake::fingerprintFor(const QString &sourceOid)
{
    if (sourceOid.isEmpty()) return QString();
    const QByteArray key = (producerId() + QLatin1Char('|') + sourceOid).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
}

QString MeshBake::fileNameFor(const QString &sourceOid)
{
    return QStringLiteral("%1.jmb").arg(sourceOid.left(16));
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
    const aiMatrix4x4 global = node->mTransformation * parent;
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
// its geometric ERROR, and the engine picks a level per object per frame from
// that error (irisgl/engine/src/OgreMesh.cpp, lodValuesFromErrors).
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
constexpr int   kMaxLevels     = 4;      ///< levels ABOVE 0. mCurrentMeshLod is a uint8; four is what §7.2 asks for.
constexpr float kRatio         = 0.5f;   ///< each level targets half the previous triangle count (the paper's step).
constexpr int   kMinTriangles  = 128;    ///< below this a level saves nothing worth a buffer — and is clusterlod's own leaf size.
constexpr float kAcceptRatio   = 0.85f;  ///< a level that could not shed 15% is topology-locked: stop, do not store it.
constexpr float kMaxRelError   = 0.05f;  ///< and stop once the error passes 5% of the mesh extent — beyond that it is a blob, not the object.
constexpr float kNormalWeight  = 0.5f;   ///< meshoptimizer's own reference weight for unit normals.
constexpr float kUvWeight      = 0.5f;   ///< the same relative priority, times extent/uvRange (see 3 above).

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

    std::vector<unsigned> prev = base;
    float accumulated = 0.0f;
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
        QVector<quint32> levelIndices;
        levelIndices.resize(int(n));
        std::memcpy(levelIndices.data(), out.data(), n * sizeof(unsigned));
        mesh->lodIndices.append(levelIndices);
        mesh->lodErrors.append(error);

        accumulated = error;
        prev.swap(out);
    }
}

}   // namespace lodchain

}   // namespace

void MeshBake::buildLodChain(const MeshPtr &mesh) { lodchain::build(mesh); }

MeshBake::Model MeshBake::buildFromScene(const SceneSource &source, const QString &filePath,
                                         const QString &fingerprint, const QString &extractDir)
{
    return buildFromScene(source.scene(), filePath, fingerprint, extractDir);
}

MeshBake::Model MeshBake::buildFromScene(const aiScene *scene, const QString &filePath,
                                         const QString &fingerprint, const QString &extractDir)
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
        auto mesh = MeshPtr(new Mesh(const_cast<aiMesh *>(m)));
        if (m->HasBones()) mesh->setSkeleton(Mesh::extractSkeleton(m, scene));
        // ATOM stage 1: the LOD chain is a product of the bake, built here and
        // nowhere else. The fallback parse path (a library with no bake yet)
        // gets no chain — which is the same "no LOD" behaviour the tree has
        // today, and one more reason a bake is worth having.
        MeshBake::buildLodChain(mesh);
        model.meshes.append(mesh);

        const unsigned aiMatIndex = m->mMaterialIndex;
        auto known = materialIndexMap.constFind(aiMatIndex);
        if (known != materialIndexMap.constEnd()) {
            materialFor[int(i)] = known.value();
            continue;
        }
        MeshMaterialData data;
        if (aiMatIndex < scene->mNumMaterials)
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
                                        const QString &extractDir)
{
    Assimp::Importer importer;
    const aiScene *scene = [&]() {
        ParseCensus::Record census(filePath);
        return importer.ReadFile(filePath.toStdString().c_str(), iris::ImportFlags::Canonical);
    }();
    if (!scene) {
        irisLog("mesh bake: assimp could not read " + filePath);
        return Model();
    }
    return buildFromScene(scene, filePath, fingerprint, extractDir);
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
