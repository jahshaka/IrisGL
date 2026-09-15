/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/scenesource.h"

#include "assimp/Importer.hpp"
#include "assimp/scene.h"

#include "assimp/config.h"
#include "assimp/mesh.h"
#include "assimp/matrix3x3.h"
#include "assimp/matrix4x4.h"
#include "assimp/quaternion.h"
#include "assimp/vector3.h"

#include "import/importflags.h"
#include "import/importsettings.h"
#include "import/modelsceneinfo.h"
#include "import/parsecensus.h"

#include <QFile>
#include <QFileInfo>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace iris
{

namespace
{

/// The ROOT PRE-MULTIPLY (SPECS/IMPORT_DIALOG_SPEC.md §4.1, the lead's audit
/// correction): T*R is applied AFTER ReadFile has run the post-processing,
/// because assimp's own ScaleProcess decomposes EVERY node's transformation
/// — the root included — and rescales its translation
/// (thirdparty/assimp ScaleProcess.cpp:75-97). A metre-valued translation
/// written before post-processing would come out as k*T.
///
/// A ROOT transform sits above the mesh nodes AND above every bone, so bone
/// offset matrices (mesh space -> bone space) are untouched and a skeleton
/// rotates as one rigid thing. Collada's own up-axis fix is the precedent
/// (ColladaLoader.cpp:190-196).
void applyRootTransform(const aiScene *scene, const ImportTransform &xf)
{
    if (!scene || !scene->mRootNode) return;
    if (!xf.hasRotation() && !xf.hasTranslation()) return;

    const Quat q = xf.rotation;
    const aiQuaternion rot(q.scalar(), q.x(), q.y(), q.z());
    aiMatrix4x4 combined(rot.GetMatrix());
    combined.a4 = ai_real(xf.translation.x());
    combined.b4 = ai_real(xf.translation.y());
    combined.c4 = ai_real(xf.translation.z());

    // const_cast: the scene belongs to the importer that just produced it and
    // is handed back const only so consumers do not edit it by accident. This
    // IS the producer half of the parse.
    aiNode *root = const_cast<aiNode *>(scene->mRootNode);
    root->mTransformation = combined * root->mTransformation;
}

/// The node chain root..mesh-holder for `meshIndex`, or false.
bool findMeshNodeChain(aiNode *node, unsigned meshIndex, QVector<aiNode *> &chain)
{
    if (!node) return false;
    chain.append(node);
    for (unsigned i = 0; i < node->mNumMeshes; ++i)
        if (node->mMeshes[i] == meshIndex) return true;
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        if (findMeshNodeChain(node->mChildren[i], meshIndex, chain)) return true;
    chain.removeLast();
    return false;
}

/// THE SINGLE-MESH FOLD (SPECS/IMPORT_DIALOG_SPEC.md §2 finding d, §4.2).
///
/// A file with ONE mesh and no bones takes the "single MeshNode, no children"
/// shortcut in every consumer (MeshNode::loadAsSceneFragment and the bake's
/// mirror of it), and that shortcut used to put the file's own mesh-node
/// pos/rot/SCALE on the fragment ROOT — the node the user places. So a
/// single-mesh file authored with a node scale arrived at scale != 1 no matter
/// what its import settings said, and after the import dialog EVERY instance
/// is supposed to read scale 1.
///
/// The transform is folded into the VERTICES instead and the chain that
/// carried it is set to identity, so all three consumers emit an identity root
/// without knowing this happened. It runs HERE, at the choke point, rather
/// than inside the bake, because the bake and the parse fallback have to agree
/// node for node (tests/meshbake compares the two trees) — one implementation,
/// not two that must not drift.
///
/// Normals and tangent frames take the INVERSE TRANSPOSE and are renormalised:
/// a non-uniform node scale shears them otherwise. Positions take the matrix
/// itself. A singular (zero-scale) transform is left alone — folding it would
/// destroy the geometry, and a degenerate node is the file's problem, not ours.
///
/// A MIRROR (negative determinant) ALSO REVERSES FACE WINDING. A node scale
/// with an odd number of negative axes turns the mesh inside out, and while the
/// transform sat on the NODE the renderer compensated (Ogre flips culling on a
/// negative node scale, `mFlipCullingOnNegativeScale`); folded into the
/// vertices there is no node left to notice, so the winding has to be reversed
/// here. assimp's own pre-transform does exactly this
/// (PretransformVertices.cpp:302-305 hands a mirrored mesh to
/// FlipWindingOrderProcess). The bitangents are negated with it: handedness is
/// part of the tangent FRAME, and the inverse transpose does not carry it.
///
/// THE WHOLE CHAIN root..mesh-node is zeroed, not just the mesh node. In a
/// one-mesh scene that is the only geometry, but a file whose chain also
/// carries a CAMERA or a LIGHT node would have those moved by it. We import
/// neither from a model file (every consumer builds a MeshNode and nothing
/// else), so nothing observes it today — recorded because a future importer
/// that did read them would inherit a silent bug.
void foldSingleMeshTransform(const aiScene *scene)
{
    if (!scene || scene->mNumMeshes != 1 || !scene->mRootNode) return;
    aiMesh *mesh = scene->mMeshes[0];
    if (!mesh || mesh->mNumBones != 0 || !mesh->HasPositions()) return;

    QVector<aiNode *> chain;
    if (!findMeshNodeChain(const_cast<aiNode *>(scene->mRootNode), 0, chain)) return;

    aiMatrix4x4 world;                               // identity
    for (aiNode *node : chain) world = world * node->mTransformation;
    if (world.IsIdentity()) return;
    if (std::fabs(double(world.Determinant())) < 1e-20) return;

    aiMatrix3x3 normalMatrix(world);
    normalMatrix.Inverse();
    normalMatrix.Transpose();

    const auto foldVectors = [&](aiVector3D *positions, aiVector3D *normals,
                                 aiVector3D *tangents, aiVector3D *bitangents, unsigned count) {
        for (unsigned v = 0; v < count; ++v) {
            if (positions) positions[v] = world * positions[v];
            if (normals) normals[v] = (normalMatrix * normals[v]).NormalizeSafe();
            if (tangents) tangents[v] = (normalMatrix * tangents[v]).NormalizeSafe();
            if (bitangents) bitangents[v] = (normalMatrix * bitangents[v]).NormalizeSafe();
        }
    };
    foldVectors(mesh->mVertices, mesh->mNormals, mesh->mTangents, mesh->mBitangents,
                mesh->mNumVertices);
    for (unsigned a = 0; a < mesh->mNumAnimMeshes; ++a) {
        aiAnimMesh *anim = mesh->mAnimMeshes[a];
        if (!anim) continue;
        foldVectors(anim->mVertices, anim->mNormals, anim->mTangents, anim->mBitangents,
                    anim->mNumVertices);
    }

    if (world.Determinant() < ai_real(0.0)) {
        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            aiFace &face = mesh->mFaces[f];
            for (unsigned i = 0; i < face.mNumIndices / 2; ++i)
                std::swap(face.mIndices[i], face.mIndices[face.mNumIndices - 1 - i]);
        }
        const auto flipHandedness = [](aiVector3D *bitangents, unsigned count) {
            if (!bitangents) return;
            for (unsigned v = 0; v < count; ++v) bitangents[v] = -bitangents[v];
        };
        flipHandedness(mesh->mBitangents, mesh->mNumVertices);
        for (unsigned a = 0; a < mesh->mNumAnimMeshes; ++a)
            if (mesh->mAnimMeshes[a])
                flipHandedness(mesh->mAnimMeshes[a]->mBitangents,
                               mesh->mAnimMeshes[a]->mNumVertices);
    }

    for (aiNode *node : chain) node->mTransformation = aiMatrix4x4();
}

}   // namespace

const aiScene *readSceneFile(Assimp::Importer &importer, const QString &filePath,
                             unsigned int flags, const ImportTransform &xf)
{
    // THE SCALE reaches assimp as its own GLOBAL_SCALE_FACTOR property, which
    // ScaleProcess composes with the file's declared scale and applies to
    // vertices, morph targets, bone offset TRANSLATIONS, node translations and
    // animation position keys — and to nothing else, so rotations come out bit
    // for bit as authored (ScaleProcess.cpp:107-196). The property is written
    // on EVERY call, including the identity one: an importer is reusable and a
    // value left over from a previous read would silently resize the next.
    double k = xf.scale;
    if (xf.overridesUnit()) {
        // A UNIT OVERRIDE needs the file's own declaration, because assimp has
        // already multiplied it in. The caller usually knows it (the asset's
        // metadata block records it as `unitScale`); when it does not, this is
        // ONE light parse — no post-processing — and it is paid only by an
        // import that says "the file's declaration is wrong".
        const double declared = xf.declaredUnitScale > 0.0
                                    ? xf.declaredUnitScale
                                    : ModelSceneInfo::readDeclaredUnitScale(filePath);
        k = xf.globalScaleFactor(declared);
    }
    importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, float(k));

    // Counted and attributed to the thread that pays for it
    // (import/parsecensus.h) — including the resource read below, which is a
    // parse like any other.
    const aiScene *scene = nullptr;
    {
        ParseCensus::Record census(filePath);
        QString resource;
        if (filePath.startsWith(QLatin1String("qrc:"))) resource = filePath.mid(3);   // "qrc:/x" -> ":/x"
        else if (filePath.startsWith(QLatin1Char(':'))) resource = filePath;
        if (resource.isEmpty()) {
            scene = importer.ReadFile(filePath.toStdString().c_str(), flags);
        } else {
            QFile file(resource);
            if (!file.open(QIODevice::ReadOnly)) {
                qWarning("readSceneFile: failed to open %s", qUtf8Printable(filePath));
                return nullptr;
            }
            const QByteArray data = file.readAll();
            const QByteArray hint = QFileInfo(resource).suffix().toLower().toLatin1();
            scene = importer.ReadFileFromMemory(data.constData(), size_t(data.size()), flags,
                                                hint.isEmpty() ? "" : hint.constData());
        }
    }
    applyRootTransform(scene, xf);
    foldSingleMeshTransform(scene);
    return scene;
}

struct SceneSource::Impl
{
    Assimp::Importer importer;
};

SceneSource::SceneSource() : d(new Impl) {}
SceneSource::~SceneSource() = default;

bool SceneSource::read(const QString &filePath, const ImportTransform &xf)
{
    return readSceneFile(d->importer, filePath, ImportFlags::Canonical, xf) != nullptr;
}

bool SceneSource::hasScene() const
{
    return d->importer.GetScene() != nullptr;
}

QString SceneSource::errorString() const
{
    return QString::fromUtf8(d->importer.GetErrorString());
}

const aiScene *SceneSource::scene() const
{
    return d->importer.GetScene();
}

Assimp::Importer &SceneSource::importer()
{
    return d->importer;
}

} // namespace iris
