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

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/version.h"

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

constexpr int kFormatVersion = 1;
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
    s << m.diffuseTexture << m.specularTexture << m.normalTexture << m.hightTexture
      << m.nodeName;
    s << qint32(m.hasEmbeddedDiffTexture) << qint32(m.hasEmbeddedSpecularTexture)
      << qint32(m.hasEmbeddedNormalTexture) << qint32(m.hasEmbeddedHightTexture);
    s << qint32(m.hasPbr);
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
    s >> m.diffuseTexture >> m.specularTexture >> m.normalTexture >> m.hightTexture
      >> m.nodeName;
    qint32 e0, e1, e2, e3, pbr;
    s >> e0 >> e1 >> e2 >> e3 >> pbr;
    m.hasEmbeddedDiffTexture = e0;
    m.hasEmbeddedSpecularTexture = e1;
    m.hasEmbeddedNormalTexture = e2;
    m.hasEmbeddedHightTexture = e3;
    m.hasPbr = pbr;
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
        s << qint32(bone->parentBone.isNull() ? -1 : index.value(bone->parentBone.data(), -1));
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
            ? bone->parentBone->inverseMeshSpacePoseMatrix * bone->meshSpacePoseMatrix
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
        qint32 boneCount = 0;
        s >> boneCount;
        if (s.status() != QDataStream::Ok || boneCount < 0 || boneCount > 1000000) return false;
        auto anim = SkeletalAnimation::create();
        anim->name = name;
        anim->source = source;
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
    return QStringLiteral("v%1|%2|assimp%3.%4.%5|flags%6")
        .arg(kFormatVersion)
        .arg(QLatin1String(JAHSHAKA_MESH_BAKE_PRODUCER_ID))
        .arg(aiGetVersionMajor()).arg(aiGetVersionMinor()).arg(aiGetVersionRevision())
        .arg(quint64(iris::ImportFlags::Canonical));
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

}   // namespace

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
                                                dir, data, extractDir);
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
    const aiScene *scene =
        importer.ReadFile(filePath.toStdString().c_str(), iris::ImportFlags::Canonical);
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
