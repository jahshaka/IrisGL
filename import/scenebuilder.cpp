/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/scenebuilder.h"

#include <QFileInfo>
#include <QDir>
#include <QScopedPointer>
#include <functional>

#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/trs.h"
#include "core/math/vec.h"
#include "core/geometry/aabb.h"
#include "core/geometry/trimesh.h"
#include "document/animation/animation.h"
#include "document/animation/skeletalanimation.h"
#include "document/assets/mesh.h"
#include "document/assets/skeleton.h"
#include "document/assets/vertexbuffer.h"
#include "document/assets/vertexlayout.h"
#include "document/materials/material.h"
#include "document/scenegraph/meshnode.h"
#include "document/scenegraph/scenenode.h"
#include "import/clipnaming.h"
#include "import/importflags.h"
#include "import/materialhelper.h"

#include "assimp/Importer.hpp"
#include "assimp/ProgressHandler.hpp"
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "assimp/mesh.h"
#include "assimp/material.h"
#include "assimp/matrix4x4.h"
#include "assimp/vector3.h"
#include "assimp/quaternion.h"

namespace iris
{

namespace {

/// The importer's progress hook, forwarded to the caller's IModelReadProgress.
/// Owned by the Importer once set (SetProgressHandler takes ownership).
class ModelProgressHandler : public Assimp::ProgressHandler
{
public:
    explicit ModelProgressHandler(IModelReadProgress *handler) : handler(handler) {}
    bool Update(float percentage) override
    {
        if (handler) handler->onProgress(percentage);
        return true;
    }
private:
    IModelReadProgress *handler = nullptr;
};

} // namespace

iris::Mat4 aiMatrixToQMatrix(aiMatrix4x4 aiMat) {
    aiVector3D pos,scale;
    aiQuaternion rot;

    aiMat.Decompose(scale,rot,pos);

    rot.Normalize();

    iris::Mat4 mat;
    mat.setToIdentity();
    mat.translate(iris::Vec3(pos.x, pos.y, pos.z));
    mat.rotate(iris::Quat(rot.w, rot.x, rot.y, rot.z));
    mat.scale(iris::Vec3(scale.x, scale.y, scale.z));

    return mat;
}

static void addVertexArray(Mesh &mesh, VertexAttribUsage usage,void* dataPtr,int size,int type,int numComponents)
{
    VertexLayout layout;
    int sizeInBytes = 0;
    switch(type) {
    case AttribTypeFloat:
    case AttribTypeInt:
        sizeInBytes = 4 * numComponents;
    }
    layout.addAttrib(usage, type, numComponents, sizeInBytes);
    auto vb = VertexBuffer::create(layout);
    vb->setData(dataPtr, size);
    mesh.addVertexBuffer(vb);
}

static void calculateBounds(Mesh &out, const aiMesh* mesh)
{
	out.aabb = AABB();
	for (unsigned int i = 0; i<mesh->mNumVertices; i++) {
		auto& vert = mesh->mVertices[i];
		out.aabb.merge(iris::Vec3(vert.x, vert.y, vert.z));
	}

	out.boundingSphere = out.aabb.getMinimalEnclosingSphere();
}

// http://ogldev.atspace.co.uk/www/tutorial38/tutorial38.html
MeshPtr meshFromAiMesh(aiMesh* mesh, bool withBones)
{
    auto out = Mesh::create();
    out->triMesh = new TriMesh();

    out->numVerts = mesh->mNumFaces*3;
    out->numFaces = mesh->mNumFaces;

    if(!mesh->HasPositions())
        return out;
        //throw QString("Mesh has no positions!!");

    addVertexArray(*out, VertexAttribUsage::Position, (void*)mesh->mVertices, sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);


    if(mesh->HasTextureCoords(0))
        addVertexArray(*out, VertexAttribUsage::TexCoord0, (void*)mesh->mTextureCoords[0], sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);
    if(mesh->HasTextureCoords(1))
        addVertexArray(*out, VertexAttribUsage::TexCoord1, (void*)mesh->mTextureCoords[1], sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);
    if(mesh->HasNormals())
        addVertexArray(*out, VertexAttribUsage::Normal, (void*)mesh->mNormals, sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);
    if(mesh->HasTangentsAndBitangents()) {
        addVertexArray(*out, VertexAttribUsage::Tangent, (void*)mesh->mTangents, sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);
        // Bitangents carry the tangent-frame handedness (glTF TANGENT.w).
        // Without them the mirror cannot tell a mirrored UV island from a
        // regular one and authored tangent frames flip.
        addVertexArray(*out, VertexAttribUsage::BiTangent, (void*)mesh->mBitangents, sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);
    }

    if (mesh->HasBones() && withBones) {
        // bone weights for skeletal animation
    #define MAX_BONE_INDICES 4
        QVector<float> boneIndices;
        boneIndices.resize(MAX_BONE_INDICES * mesh->mNumVertices);
        boneIndices.fill(0);
        QVector<float> boneWeights;
        boneWeights.resize(MAX_BONE_INDICES * mesh->mNumVertices);
        boneWeights.fill(0);


        for (unsigned i = 0;i<mesh->mNumBones; i++) {
            auto bone = mesh->mBones[i];

            for (unsigned j = 0;j<bone->mNumWeights ; j++) {
                auto weight = bone->mWeights[j];
                auto baseIndex = weight.mVertexId * MAX_BONE_INDICES;
                //qDebug() << weight.mVertexId << " - " << i << " - " << weight.mWeight;
                // find empty slot and set weight
                for(unsigned k = 0; k<MAX_BONE_INDICES; k++) {
                    if (baseIndex + k < (unsigned)boneWeights.size()) { //just in case
                        if(boneWeights[baseIndex + k] == 0) {
                            // an empty weight means an empty slot
                            boneIndices[baseIndex + k] = i;// bone index in array
                            boneWeights[baseIndex + k] = weight.mWeight;
                            break;
                        }
                    } else {
                        //qDebug() << "Invalid vertex index "<<baseIndex + k;
                    }
                }
            }
        }

//        for ( auto i =0 ; i < boneWeights.size(); i++) {
//            qDebug() << boneIndices[i] << " - " << boneWeights[i];
//        }

        addVertexArray(*out, VertexAttribUsage::BoneIndices, (void*)boneIndices.data(), sizeof(float) * boneIndices.size(), AttribTypeFloat, MAX_BONE_INDICES);
        addVertexArray(*out, VertexAttribUsage::BoneWeights, (void*)boneWeights.data(), sizeof(float) * boneWeights.size(), AttribTypeFloat, MAX_BONE_INDICES);
    }

    // Assimp doesnt give the indices in an array
    // So some calculation still has to be done
    QVector<unsigned int> indices;
    indices.reserve(mesh->mNumFaces * 3);
    out->triMesh->triangles.reserve(mesh->mNumFaces);
    for(unsigned i = 0; i < mesh->mNumFaces; i++)
    {

        auto face = mesh->mFaces[i];

        if (face.mNumIndices!=3)
            continue;

        indices.append(face.mIndices[0]);
        indices.append(face.mIndices[1]);
        indices.append(face.mIndices[2]);

        auto a = mesh->mVertices[face.mIndices[0]];
        auto b = mesh->mVertices[face.mIndices[1]];
        auto c = mesh->mVertices[face.mIndices[2]];

        out->triMesh->addTriangle(iris::Vec3(a.x, a.y, a.z),
                             iris::Vec3(b.x, b.y, b.z),
                             iris::Vec3(c.x, c.y, c.z));
    }

    out->usesIndexBuffer = true;
    auto idxBuffer = IndexBuffer::create();
    idxBuffer->setData(indices.data(), sizeof(unsigned int) * indices.size());
    out->setIndexBuffer(idxBuffer);

    // the true size
    out->numVerts = indices.size();

    out->setPrimitiveMode(PrimitiveMode::Triangles);
	calculateBounds(*out, mesh);
    return out;
}

SkeletonPtr extractSkeleton(const aiMesh *mesh, const aiScene *scene)
{
    if (mesh->mNumBones == 0)
        return SkeletonPtr();

    // create skeleton and add animations
    auto skel = Skeleton::create();
    for (unsigned i = 0;i<mesh->mNumBones; i++) {
        auto meshBone = mesh->mBones[i];

        auto bone = Bone::create(std::string(meshBone->mName.C_Str()));
        bone->inverseMeshSpacePoseMatrix = aiMatrixToQMatrix(meshBone->mOffsetMatrix);
        bone->meshSpacePoseMatrix = bone->inverseMeshSpacePoseMatrix.inverted();

        skel->addBone(bone);
    }

    // Evaluate the bone hierarchy over the aiNode tree, linking each bone to its
    // NEAREST BONE ANCESTOR — not to its immediate parent node.
    //
    // This used to link a bone only when its aiNode's DIRECT parent was also a
    // bone, which is empty for every pivot-preserving FBX: assimp defaults
    // AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS to true (nothing in this tree ever
    // sets it), so a chain of `$AssimpFbx$_Translation/_Rotation/...` nodes sits
    // between every pair of real bones and no link was ever made. A 67-bone
    // Mixamo rig came back with 67 parentless bones (AVATAR_MODULE_SPEC §0.1),
    // and anything reading the hierarchy — the glTF exporter's joint tree, and
    // the engine skeleton GPU skinning builds — silently got a flat rig.
    //
    // Skipping intermediate non-bone nodes is exactly what the overlay's
    // "nearest ancestor that is also a bone" walk does, done once at import
    // instead of per consumer. For a rig with no intermediate nodes the result
    // is identical to the old behaviour (nearest bone ancestor == parent bone).
    std::function<void(aiNode*, const BonePtr&)> evalChildren;
    evalChildren = [&skel, &evalChildren](aiNode *node, const BonePtr &ancestor) {
        auto bone = skel->getBone(std::string(node->mName.C_Str()));
        // A bone may appear twice in a node tree only if the file is malformed;
        // don't re-parent one that already has a parent.
        if (!!bone && !!ancestor && bone->parentBone.expired() && bone != ancestor)
            ancestor->addChild(bone);
        const BonePtr &nextAncestor = !!bone ? bone : ancestor;
        for (unsigned i = 0; i < node->mNumChildren; i++)
            evalChildren(node->mChildren[i], nextAncestor);
    };

    evalChildren(scene->mRootNode, BonePtr());

    // THE BIND LOCAL of every bone (ANIMATION_ENGINE_MIGRATION_SPEC §1.5 F1).
    //
    // `Bone::bindingPos/bindingRot/bindingScale` were never written on the live
    // import path — the only writers were in irisgl/import/modelloader.cpp,
    // which the editor's route does not go through. Two consumers read them and
    // both got the default-constructed values: `Skeleton::applyAnimation(anim,
    // time)` (dead on the live path), and the glTF EXPORTER, which writes them
    // as every joint's TRS — so every rig we have ever exported to the web came
    // out structurally right and bind-posed WRONG: translation (0,0,0),
    // identity rotation, and a zero scale saved only by a guard in the writer.
    //
    // The correct value is the bone's transform local to its PARENT BONE, which
    // is exactly what SceneMirror::toSkeletonDesc derives for the engine:
    // FK over these reproduces meshSpacePoseMatrix, whose inverse is assimp's
    // offset matrix. One quantity, three consumers, computed once here.
    for (const auto &bone : skel->bones) {
        const iris::Mat4 bindLocal = !bone->parentBone.expired()
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

QMap<QString, SkeletalAnimationPtr> extractAnimations(const aiScene *scene, QString source)
{
    QMap<QString, SkeletalAnimationPtr> anims;

    for (unsigned i = 0; i<scene->mNumAnimations; i++) {
        auto anim = scene->mAnimations[i];

        // Key times are stored in SECONDS (SKELETAL_PLAYBACK_SPEC S2):
        // assimp gives raw ticks + mTicksPerSecond (1000 for glTF/FBX,
        // 24/30 for many FBX/Collada exports; 0 means "unknown" — assimp's
        // documented convention is to assume 25). The old code stored raw
        // ticks and compensated with a `length > 60 → time × 1000` hack in
        // SceneNode::updateAnimation, which mis-played any non-ms clip.
        const double ticksPerSecond =
            anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0;

        // Clip names are kept and made unique by THE one naming rule
        // (import/clipnaming.h) — shared with the import dialog's pre-read,
        // which lists the very names the `clips` import setting filters on.
        const QString animName = clipNameFor(QString(anim->mName.C_Str()), i, anims);

        auto skelAnim = SkeletalAnimation::create();
        skelAnim->name = animName.toStdString();
        skelAnim->source = source.toStdString();
        // The duration the FILE declares, kept beside the keys: it is the only
        // record of a one-frame clip's length once FindInvalidData has merged
        // its identical keys (smoke L10 item 2; SkeletalAnimation::declaredLength).
        skelAnim->declaredLength =
            anim->mDuration > 0.0 ? float(anim->mDuration / ticksPerSecond) : 0.0f;

        for (unsigned j = 0; j<anim->mNumChannels; j++) {
            auto nodeAnim = anim->mChannels[j];

            auto nodeName = QString(nodeAnim->mNodeName.C_Str());
            auto boneAnim = new BoneAnimation();

            // extract tracks (tick → second conversion at the source)
            for (unsigned k = 0; k<nodeAnim->mNumPositionKeys; k++) {
                auto key = nodeAnim->mPositionKeys[k];
                boneAnim->posKeys->addKey(iris::Vec3(key.mValue.x, key.mValue.y, key.mValue.z), key.mTime / ticksPerSecond);
            }

            for (unsigned k = 0; k<nodeAnim->mNumRotationKeys; k++) {
                auto key = nodeAnim->mRotationKeys[k];
                boneAnim->rotKeys->addKey(iris::Quat(key.mValue.w, key.mValue.x, key.mValue.y, key.mValue.z), key.mTime / ticksPerSecond);
            }

            for (unsigned k = 0; k<nodeAnim->mNumScalingKeys; k++) {
                auto key = nodeAnim->mScalingKeys[k];
                boneAnim->scaleKeys->addKey(iris::Vec3(key.mValue.x, key.mValue.y, key.mValue.z), key.mTime / ticksPerSecond);
            }

            skelAnim->addBoneAnimation(nodeName.toStdString(), boneAnim);
        }

        anims.insert(animName, skelAnim);
    }

    return anims;
}

/**
 * Recursively builds a SceneNode/MeshNode heirarchy from the aiScene of the loaded model
 * todo: read and apply material data
 * @param scene
 * @param node
 * @return
 */
// Accumulated (root -> node) transform of the first aiNode referencing
// `meshIndex`. assimp's matrices are row-major, so composition is
// child * parent.
static bool _findMeshNodeTransform(const aiNode* node, unsigned meshIndex,
                                   const aiMatrix4x4& parent, aiMatrix4x4& out)
{
    // parent * child: aiMatrix4x4 multiplies COLUMN vectors, so the ancestor
    // goes on the left (IMPORT-1: it read child * parent, which only agreed
    // with ModelSceneInfo's own walk while every ancestor was identity — and
    // an import transform pre-multiplied onto the root is not).
    const aiMatrix4x4 global = parent * node->mTransformation;
    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        if (node->mMeshes[i] == meshIndex) { out = global; return true; }
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        if (_findMeshNodeTransform(node->mChildren[i], meshIndex, global, out))
            return true;
    }
    return false;
}

// The single-mesh shortcut in loadAsSceneFragment used to return a MeshNode
// at identity, silently DROPPING the authored root/node transform (a glb
// whose one mesh sits under a scaled root imported at the wrong size while
// the same file's multi-mesh sibling kept it). Apply the accumulated
// transform of the node that references the mesh.
static void _applyMeshNodeTransform(const aiScene* scene, MeshNodePtr node)
{
    aiMatrix4x4 xform;
    if (!_findMeshNodeTransform(scene->mRootNode, 0, aiMatrix4x4(), xform)) return;
    aiVector3D pos, scale;
    aiQuaternion rot;
    xform.Decompose(scale, rot, pos);
    node->setLocalPos(iris::Vec3(pos.x, pos.y, pos.z));
    node->setLocalScale(iris::Vec3(scale.x, scale.y, scale.z));
    node->setLocalRot(iris::Quat(rot.w, rot.x, rot.y, rot.z));
}

/// The file's clips, through the import settings' `clips` switch
/// (import/importsettings.h §4.3). Filtered AFTER extraction, on the names the
/// rest of the app shows — extractAnimations uniquifies raw names, and a filter
/// written against the raw ones would miss. MeshBake::buildFromScene filters
/// the identical way, because the bake and this path must agree.
static QMap<QString, SkeletalAnimationPtr> filteredClips(const aiScene *scene,
                                                         const QString &filePath,
                                                         const ImportTransform &xf)
{
    auto anims = extractAnimations(scene, filePath);
    if (xf.clips && xf.clipNames.isEmpty()) return anims;
    QMap<QString, SkeletalAnimationPtr> kept;
    for (auto it = anims.constBegin(); it != anims.constEnd(); ++it)
        if (xf.wantsClip(it.key())) kept.insert(it.key(), it.value());
    return kept;
}

std::shared_ptr<iris::SceneNode> _buildScene(const aiScene* scene,
											aiNode* node,
											SceneNodePtr rootBone,
											QString filePath,
											std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
											const QString& extractDir = QString(),
											const ImportTransform &xf = ImportTransform())
{
    std::shared_ptr<iris::SceneNode> sceneNode;

    // if this node only has one child then make sceneNode a meshnode and add mesh to it
    if (node->mNumMeshes == 1) {
        auto meshNode = iris::MeshNode::create();
        auto mesh = scene->mMeshes[node->mMeshes[0]];

        // objects like Bezier curves have no vertex positions in the aiMesh
        // aside from that, iris currently only renders meshes
        if (mesh->HasPositions()) {
            // THE TUNING SWITCHES (import/importsettings.h §4.3), applied to
            // what is BUILT. The bake does exactly this, in the same order —
            // tests/meshbake compares the two trees node for node.
            auto meshObj = meshFromAiMesh(mesh, xf.skeleton);
            if (xf.skeleton) meshObj->setSkeleton(extractSkeleton(mesh, scene));

            meshNode->setMesh(meshObj);
            meshNode->name = std::string(mesh->mName.C_Str());
            meshNode->meshPath = filePath;
            meshNode->meshIndex = node->mMeshes[0];

            // mesh->mMaterialIndex is always at least 0
            auto m = scene->mMaterials[mesh->mMaterialIndex];
            auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

            MeshMaterialData meshMat;
            if (xf.materials)
                MaterialHelper::extractMaterialData(scene, m, dir, meshMat, extractDir, filePath, xf.faceCulling);
            auto mat = createMaterialFunc(meshObj, meshMat);
            if (!!mat) meshNode->setMaterial(mat);
            if (meshMat.twoSided) meshNode->setFaceCullingMode(FaceCullingMode::None);   // CULL-MODE-1
        }

        meshNode->rootBone = rootBone;
        sceneNode = meshNode;
    }
    else {
        //otherwise, add meshes as child nodes
        sceneNode = std::shared_ptr<iris::SceneNode>(new iris::SceneNode());
        sceneNode->name = std::string(node->mName.C_Str());

        for (unsigned i = 0; i < node->mNumMeshes; i++) {
            auto mesh = scene->mMeshes[node->mMeshes[i]];
            auto meshObj = meshFromAiMesh(mesh, xf.skeleton);
            if (xf.skeleton) meshObj->setSkeleton(extractSkeleton(mesh, scene));

            auto meshNode = iris::MeshNode::create();
            meshNode->name = std::string(mesh->mName.C_Str());
            meshNode->meshPath = filePath;
            meshNode->meshIndex = node->mMeshes[i];

            meshNode->setMesh(meshObj);
            sceneNode->addChild(meshNode);

            //apply material
            auto m = scene->mMaterials[mesh->mMaterialIndex];
            auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

            MeshMaterialData meshMat;
            if (xf.materials)
                MaterialHelper::extractMaterialData(scene, m, dir, meshMat, extractDir, filePath, xf.faceCulling);
            auto mat = createMaterialFunc(meshObj, meshMat);
            if (!!mat) meshNode->setMaterial(mat);
            if (meshMat.twoSided) meshNode->setFaceCullingMode(FaceCullingMode::None);   // CULL-MODE-1
        }
    }

    //extract transform
    aiVector3D pos,scale;
    aiQuaternion rot;

    //auto transform = node->mTransformation;
    node->mTransformation.Decompose(scale,rot,pos);
    sceneNode->setLocalPos(iris::Vec3(pos.x,pos.y,pos.z));
    sceneNode->setLocalScale(iris::Vec3(scale.x,scale.y,scale.z));

    sceneNode->setLocalRot(iris::Quat(rot.w,rot.x,rot.y,rot.z));

    // this is probably the first node in the hierarchy, set it as the rootBone
    if (!rootBone) rootBone = sceneNode;

    for (unsigned i = 0 ;i < node->mNumChildren; i++) {
        auto child = _buildScene(scene, node->mChildren[i], rootBone, filePath, createMaterialFunc, extractDir, xf);
        sceneNode->addChild(child, false);
    }

    sceneNode->setAttached(true);
    return sceneNode;
}

std::shared_ptr<iris::SceneNode>
loadAsSceneFragment(QString filePath,
                              std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
                              SceneSource *scene_, IModelReadProgress* progressReader,
                              const QString &extractDir, const ImportTransform &xf)
{
    // scene_ defaults to null in the declaration but was dereferenced
    // unconditionally — every caller had to allocate a SceneSource just to
    // avoid the crash. A local importer serves callers that don't need the
    // aiScene to outlive the call (the returned graph owns copies of
    // everything).
    QScopedPointer<SceneSource> localSource;
    if (!scene_) {
        localSource.reset(new SceneSource());
        scene_ = localSource.data();
    }

    scene_->importer().SetProgressHandler(new ModelProgressHandler(progressReader));
    // The session entry's parse (Studio's ProjectAssets::registerSessionAsset
    // on a bake-and-prewarm miss), counted by thread and carrying the ASSET's
    // import transform — through the choke point, import/scenesource.h.
    const aiScene *scene = readSceneFile(scene_->importer(), filePath,
                                         iris::ImportFlags::Canonical, xf);

    // ReadFile returns null on failure (corrupt file, or an importer feature
    // that is not compiled in, e.g. KHR_draco_mesh_compression): dereferencing
    // it segfaulted. Fail like the aiScene overload below does, with the
    // assimp error on the log so the caller can surface a message.
    if (scene == nullptr) {
        qWarning("loadAsSceneFragment: assimp failed to load %s: %s",
                 qUtf8Printable(filePath), scene_->importer().GetErrorString());
        return std::shared_ptr<iris::MeshNode>(nullptr);
    }
    if (scene->mNumMeshes == 0) return std::shared_ptr<iris::MeshNode>(nullptr);
    // AVATAR_MODULE_SPEC "ITEM ZERO" (fix A): the single-mesh shortcut below
    // builds ONE MeshNode and no child SceneNodes at all. A clip's channels are
    // matched to SCENE NODES by name — that was true of the document evaluator
    // and it is still true of clip translation (iris::ClipExtractor composes a
    // bone's chain out of the scene nodes between it and its parent bone) — so
    // a skinned file taking the shortcut has no node for any channel to land
    // on and EVERY bone stays at bind: the clip clock advances and the
    // character never moves. Silently. Every Mixamo
    // export is single-mesh, so "no single-mesh rig can animate" was the whole
    // of it.
    //
    // The multi-mesh path already builds the aiNode tree (that tree IS the bone
    // hierarchy — including the `$AssimpFbx$` pivot nodes most FBX channels are
    // named after), so a skinned single-mesh file just takes that path too. The
    // guard keeps the blast radius at files that animate not at all today:
    // unskinned single-mesh models (props, thumbnails, the sample scenes) keep
    // the shortcut, its transform fix and their exact node shape.
    const bool singleMeshShortcut =
        scene->mNumMeshes == 1 && scene->mMeshes[0]->mNumBones == 0;
    if (singleMeshShortcut) {
        auto mesh = scene->mMeshes[0];
        auto node = iris::MeshNode::create();

        auto meshObj = meshFromAiMesh(mesh, xf.skeleton);

        //todo: use relative path from scene root
        auto anims = filteredClips(scene, filePath, xf);
        for (auto animName : anims.keys()) {
            // meshObj->addSkeletalAnimation(animName, anims[animName]);
            auto anim = Animation::createFromSkeletalAnimation(anims[animName]);
            node->addAnimation(anim);
            // The FIRST clip becomes the active one, not the last. This loop
            // used to call setAnimation on every iteration, so the clip a
            // freshly imported model played was whichever name sorted LAST
            // (anims is a QMap) — and for a Mixamo character download that is
            // "mixamo.com", the single-frame T-POSE. Every such character
            // therefore stood still in the editor by default, with a real clip
            // sitting unused in the list. The Avatar page already worked around
            // this on its own load path.
            if (node->getAnimations().size() == 1) node->setAnimation(anim);
        }

        if (xf.skeleton) meshObj->setSkeleton(extractSkeleton(mesh, scene));

        node->setMesh(meshObj);
        node->meshPath = filePath;
        node->meshIndex = 0;

        auto m = scene->mMaterials[mesh->mMaterialIndex];
        auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

        MeshMaterialData meshMat;
        if (xf.materials)
            MaterialHelper::extractMaterialData(scene, m, dir, meshMat, extractDir, filePath, xf.faceCulling);
        auto mat = createMaterialFunc(meshObj, meshMat);
        if (!!mat) node->setMaterial(mat);
        if (meshMat.twoSided) node->setFaceCullingMode(FaceCullingMode::None);   // CULL-MODE-1

        _applyMeshNodeTransform(scene, node);

        return node;
    }

    auto node = _buildScene(scene, scene->mRootNode, SceneNodePtr(), filePath, createMaterialFunc, extractDir, xf);
    node->setAttached(false); // root of object shouldnt be attached

    // extract animations and add them one by one
    // todo: use relative path from scene root (Nic)
    auto anims = filteredClips(scene, filePath, xf);
    for (auto animName : anims.keys()) {
        auto anim = Animation::createFromSkeletalAnimation(anims[animName]);
        node->addAnimation(anim);
        // The FIRST clip, not the alphabetically last one — see above.
        if (node->getAnimations().size() == 1) node->setAnimation(anim);
    }

    node->applyDefaultPose();

    return node;
}

std::shared_ptr<iris::SceneNode>
loadAsSceneFragment(
	const QString &filePath,
	const aiScene* scene_,
	std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
	const QString &extractDir, const ImportTransform &xf)
{
	const aiScene *scene = scene_;

	if (scene == nullptr)
		return std::shared_ptr<iris::MeshNode>(nullptr);
	if (scene->mNumMeshes == 0) return std::shared_ptr<iris::MeshNode>(nullptr);
	// Same ITEM ZERO fix as the path-based overload above, for the same reason:
	// the single-mesh shortcut builds ONE MeshNode and no child SceneNodes, and
	// pose evaluation is name-matched over the scene-node hierarchy — so a
	// skinned file taking this shortcut has one entry in that map, every bone
	// stays at identity, and the character never moves while its clip clock
	// advances. Silently.
	//
	// This overload is the LIBRARY/IMPORT side (assetwidget.cpp:359 dropping an
	// Object asset into the scene, projectassets.cpp:126 resolving a pinned
	// project asset), so a rigged character added from the library was frozen
	// exactly the way one loaded from disk was.
	//
	// Guarded on mNumBones, so unskinned single-mesh models — every prop,
	// thumbnail and sample scene — keep the shortcut, its transform fix and
	// their exact node shape.
	const bool singleMeshShortcut =
		scene->mNumMeshes == 1 && scene->mMeshes[0]->mNumBones == 0;
	if (singleMeshShortcut) {
		auto mesh = scene->mMeshes[0];
		auto node = iris::MeshNode::create();

		auto meshObj = meshFromAiMesh(mesh, xf.skeleton);

		//todo: use relative path from scene root
		auto anims = filteredClips(scene, filePath, xf);
		for (auto animName : anims.keys()) {
			// meshObj->addSkeletalAnimation(animName, anims[animName]);
			auto anim = Animation::createFromSkeletalAnimation(anims[animName]);
			node->addAnimation(anim);
			// The FIRST clip, not the alphabetically last one — see above.
			if (node->getAnimations().size() == 1) node->setAnimation(anim);
		}

		if (xf.skeleton) meshObj->setSkeleton(extractSkeleton(mesh, scene));

		node->setMesh(meshObj);
		node->meshPath = filePath;
		node->meshIndex = 0;

		auto m = scene->mMaterials[mesh->mMaterialIndex];
		auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

		MeshMaterialData meshMat;
        if (xf.materials)
            MaterialHelper::extractMaterialData(scene, m, dir, meshMat, extractDir, filePath, xf.faceCulling);
		auto mat = createMaterialFunc(meshObj, meshMat);
		if (!!mat) node->setMaterial(mat);
		if (meshMat.twoSided) node->setFaceCullingMode(FaceCullingMode::None);   // CULL-MODE-1

		_applyMeshNodeTransform(scene, node);

		return node;
	}

	auto node = _buildScene(scene, scene->mRootNode, SceneNodePtr(), filePath, createMaterialFunc, extractDir, xf);
	node->setAttached(false); // root of object shouldnt be attached

							  // extract animations and add them one by one
							  // todo: use relative path from scene root (Nic)
	auto anims = filteredClips(scene, filePath, xf);
	for (auto animName : anims.keys()) {
		auto anim = Animation::createFromSkeletalAnimation(anims[animName]);
		node->addAnimation(anim);
		// The FIRST clip, not the alphabetically last one — see above.
		if (node->getAnimations().size() == 1) node->setAnimation(anim);
	}

	node->applyDefaultPose();

	return node;
}

}
