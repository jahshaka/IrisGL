/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "document/scenegraph/meshnode.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QDir>

#include "irisglfwd.h"
#include "core/properties/property.h"
#include "document/assets/mesh.h"
#include "assimp/postprocess.h"
#include "import/importflags.h"
#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/mesh.h"
#include "assimp/material.h"
#include "assimp/matrix4x4.h"
#include "assimp/vector3.h"
#include "assimp/quaternion.h"

#include "document/assets/vertexlayout.h"
#include "document/materials/defaultmaterial.h"
#include "document/materials/custommaterial.h"
#include "import/materialhelper.h"
#include "document/animation/animableproperty.h"
#include "document/animation/animation.h"

#include "document/scenegraph/scene.h"
#include "document/scenegraph/scenenode.h"
#include "core/irisutils.h"
#include "document/animation/animableproperty.h"

#include "document/assets/skeleton.h"

namespace iris
{

MeshNode::MeshNode() {
    sceneNodeType = SceneNodeType::Mesh;

    faceCullingMode = FaceCullingMode::DefinedInMaterial;
}

// @todo: cleanup previous mesh item
FaceCullingMode MeshNode::getFaceCullingMode() const
{
    return faceCullingMode;
}

void MeshNode::setFaceCullingMode(const FaceCullingMode &value)
{
    faceCullingMode = value;
}

QList<Property*> MeshNode::getProperties()
{
    auto props = SceneNode::getProperties();

    // meshPath/meshIndex are READ-ONLY through this reflection: writing them
    // would have to reload the mesh (and, for meshIndex, re-resolve a submesh
    // inside a model file), which is an import-time operation with asset-manager
    // side effects. setPropertyValue therefore refuses them; use setMesh().
    // There is no StringProperty in core/properties/property.h — FileProperty is
    // the QString-valued Property.
    auto pathProp = new FileProperty();
    pathProp->displayName = "Mesh Path";
    pathProp->name = "meshPath";
    pathProp->value = meshPath;
    pathProp->readOnly = true;      // matches the setPropertyValue refusal below
    props.append(pathProp);

    auto intProp = new IntProperty();
    intProp->displayName = "Mesh Index";
    intProp->name = "meshIndex";
    intProp->value = meshIndex;
    intProp->readOnly = true;
    props.append(intProp);

    intProp = new IntProperty();
    intProp->displayName = "Face Culling Mode";
    intProp->name = "faceCullingMode";
    intProp->value = static_cast<int>(faceCullingMode);
    props.append(intProp);

    // @todo: extract properties from material
    return props;
}

QVariant MeshNode::getPropertyValue(QString valueName)
{
    if (valueName == "meshPath")        return meshPath;
    if (valueName == "meshIndex")       return meshIndex;
    if (valueName == "faceCullingMode") return static_cast<int>(faceCullingMode);

    return SceneNode::getPropertyValue(valueName);
}

bool MeshNode::setPropertyValue(QString valueName, const QVariant &value)
{
    // Read-only: see getProperties(). Returning false makes the caller report
    // "rejected property" instead of silently reloading geometry.
    if (valueName == "meshPath")  return false;
    if (valueName == "meshIndex") return false;
    if (valueName == "faceCullingMode") {
        setFaceCullingMode(static_cast<FaceCullingMode>(value.toInt()));
        return true;
    }

    return SceneNode::setPropertyValue(valueName, value);
}

void MeshNode::setMesh(QString source)
{
    mesh = Mesh::loadMesh(source);
    meshPath = source;
    meshIndex = 0;
    adoptSkeletonFromMesh();
}

//should not be used on plain scene meshes
void MeshNode::setMesh(MeshPtr mesh)
{
    this->mesh = mesh;
    adoptSkeletonFromMesh();
}

// GPU_SKINNING_SPEC §7. The mesh asset's skeleton is the rig template and is
// SHARED (createDuplicate hands the duplicate the same MeshPtr); pose state on
// it means two avatars of one rig fight over one boneTransforms array and the
// last writer per frame wins. Every MeshNode gets its own clone here — which
// is also what makes createDuplicate correct for free, since it goes through
// setMesh.
void MeshNode::adoptSkeletonFromMesh()
{
    if (!!mesh && mesh->hasSkeleton())
        skeleton = mesh->getSkeleton()->clone();
    else
        skeleton.reset();
}

MeshPtr MeshNode::getMesh()
{
    return mesh;
}

void MeshNode::setMaterial(MaterialPtr material)
{
    this->material = material;

	if (!!mesh) {
		if (this->mesh->hasSkeleton()) {
			material->enableFlag("SKINNING_ENABLED");
		}
		else {
			material->disableFlag("SKINNING_ENABLED");
		}
	}
}

float MeshNode::getMeshRadius()
{
    float scaleX = globalTransform.column(0).toVector3D().length();
    float scaleY = globalTransform.column(1).toVector3D().length();
    float scaleZ = globalTransform.column(2).toVector3D().length();

    return qMax(qMax(scaleX, scaleY), scaleZ);
}

BoundingSphere MeshNode::getTransformedBoundingSphere()
{
    BoundingSphere boundingSphere;
    boundingSphere.pos = this->globalTransform * mesh->boundingSphere.pos;
    boundingSphere.radius = mesh->boundingSphere.radius * getMeshRadius();
    return boundingSphere;
}

QJsonObject readJahShader(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        qWarning("readJahShader: failed to open %s", qUtf8Printable(filePath));

    auto data = file.readAll();
    file.close();
    return QJsonDocument::fromJson(data).object();
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
// child * parent (same convention as ModelLoader::extractMeshesFromScene).
static bool _findMeshNodeTransform(const aiNode* node, unsigned meshIndex,
                                   const aiMatrix4x4& parent, aiMatrix4x4& out)
{
    const aiMatrix4x4 global = node->mTransformation * parent;
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

QSharedPointer<iris::SceneNode> _buildScene(const aiScene* scene,
											aiNode* node,
											SceneNodePtr rootBone,
											QString filePath,
											std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
											const QString& extractDir = QString())
{
    QSharedPointer<iris::SceneNode> sceneNode;

    // if this node only has one child then make sceneNode a meshnode and add mesh to it
    if (node->mNumMeshes == 1) {
        auto meshNode = iris::MeshNode::create();
        auto mesh = scene->mMeshes[node->mMeshes[0]];

        // objects like Bezier curves have no vertex positions in the aiMesh
        // aside from that, iris currently only renders meshes
        if (mesh->HasPositions()) {
            auto meshObj = MeshPtr(new Mesh(mesh));
            auto skel = Mesh::extractSkeleton(mesh, scene);
            meshObj->setSkeleton(skel);

            meshNode->setMesh(meshObj);
            meshNode->name = QString(mesh->mName.C_Str());
            meshNode->meshPath = filePath;
            meshNode->meshIndex = node->mMeshes[0];

            // mesh->mMaterialIndex is always at least 0
            auto m = scene->mMaterials[mesh->mMaterialIndex];
            auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

            MeshMaterialData meshMat;
            MaterialHelper::extractMaterialData(scene, m, dir, meshMat, extractDir);
            auto mat = createMaterialFunc(meshObj, meshMat);
            if (!!mat) meshNode->setMaterial(mat);
        }

        meshNode->rootBone = rootBone;
        sceneNode = meshNode;
    }
    else {
        //otherwise, add meshes as child nodes
        sceneNode = QSharedPointer<iris::SceneNode>(new iris::SceneNode());
        sceneNode->name = QString(node->mName.C_Str());

        for (unsigned i = 0; i < node->mNumMeshes; i++) {
            auto mesh = scene->mMeshes[node->mMeshes[i]];
            auto meshObj = MeshPtr(new Mesh(mesh));
            auto skel = Mesh::extractSkeleton(mesh, scene);
            meshObj->setSkeleton(skel);

            auto meshNode = iris::MeshNode::create();
            meshNode->name = QString(mesh->mName.C_Str());
            meshNode->meshPath = filePath;
            meshNode->meshIndex = node->mMeshes[i];

            meshNode->setMesh(meshObj);
            sceneNode->addChild(meshNode);

            //apply material
            auto m = scene->mMaterials[mesh->mMaterialIndex];
            auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

            MeshMaterialData meshMat;
            MaterialHelper::extractMaterialData(scene, m, dir, meshMat, extractDir);
            auto mat = createMaterialFunc(meshObj, meshMat);
            if (!!mat) meshNode->setMaterial(mat);
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
        auto child = _buildScene(scene, node->mChildren[i], rootBone, filePath, createMaterialFunc, extractDir);
        sceneNode->addChild(child, false);
    }

    sceneNode->setAttached(true);
    return sceneNode;
}

QSharedPointer<iris::SceneNode>
MeshNode::loadAsSceneFragment(QString filePath,
                              std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
                              SceneSource *scene_, IModelReadProgress* progressReader,
                              const QString &extractDir)
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

    ModelProgressHandler *handle = new ModelProgressHandler();
    handle->setHandler(progressReader);

    scene_->importer.SetProgressHandler(handle);
    const aiScene *scene = scene_->importer.ReadFile(filePath.toStdString().c_str(), iris::ImportFlags::Canonical);

    // ReadFile returns null on failure (corrupt file, or an importer feature
    // that is not compiled in, e.g. KHR_draco_mesh_compression): dereferencing
    // it segfaulted. Fail like the aiScene overload below does, with the
    // assimp error on the log so the caller can surface a message.
    if (scene == nullptr) {
        qWarning("loadAsSceneFragment: assimp failed to load %s: %s",
                 qUtf8Printable(filePath), scene_->importer.GetErrorString());
        return QSharedPointer<iris::MeshNode>(nullptr);
    }
    if (scene->mNumMeshes == 0) return QSharedPointer<iris::MeshNode>(nullptr);
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

        auto meshObj = MeshPtr(new Mesh(mesh));

        //todo: use relative path from scene root
        auto anims = Mesh::extractAnimations(scene, filePath);
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

        auto skel = Mesh::extractSkeleton(mesh, scene);
        meshObj->setSkeleton(skel);

        node->setMesh(meshObj);
        node->meshPath = filePath;
        node->meshIndex = 0;

        auto m = scene->mMaterials[mesh->mMaterialIndex];
        auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

        MeshMaterialData meshMat;
        MaterialHelper::extractMaterialData(scene, m, dir, meshMat, extractDir);
        auto mat = createMaterialFunc(meshObj, meshMat);
        if (!!mat) node->setMaterial(mat);

        _applyMeshNodeTransform(scene, node);

        return node;
    }

    auto node = _buildScene(scene, scene->mRootNode, SceneNodePtr(), filePath, createMaterialFunc, extractDir);
    node->setAttached(false); // root of object shouldnt be attached

    // extract animations and add them one by one
    // todo: use relative path from scene root (Nic)
    auto anims = Mesh::extractAnimations(scene, filePath);
    for (auto animName : anims.keys()) {
        auto anim = Animation::createFromSkeletalAnimation(anims[animName]);
        node->addAnimation(anim);
        // The FIRST clip, not the alphabetically last one — see above.
        if (node->getAnimations().size() == 1) node->setAnimation(anim);
    }

    node->applyDefaultPose();

    return node;
}

QSharedPointer<iris::SceneNode>
MeshNode::loadAsSceneFragment(
	const QString &filePath,
	const aiScene* scene_,
	std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
	const QString &extractDir)
{
	const aiScene *scene = scene_;

	if (scene == nullptr)
		return QSharedPointer<iris::MeshNode>(nullptr);
	if (scene->mNumMeshes == 0) return QSharedPointer<iris::MeshNode>(nullptr);
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

		auto meshObj = MeshPtr(new Mesh(mesh));

		//todo: use relative path from scene root
		auto anims = Mesh::extractAnimations(scene, filePath);
		for (auto animName : anims.keys()) {
			// meshObj->addSkeletalAnimation(animName, anims[animName]);
			auto anim = Animation::createFromSkeletalAnimation(anims[animName]);
			node->addAnimation(anim);
			// The FIRST clip, not the alphabetically last one — see above.
			if (node->getAnimations().size() == 1) node->setAnimation(anim);
		}

		auto skel = Mesh::extractSkeleton(mesh, scene);
		meshObj->setSkeleton(skel);

		node->setMesh(meshObj);
		node->meshPath = filePath;
		node->meshIndex = 0;

		auto m = scene->mMaterials[mesh->mMaterialIndex];
		auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

		MeshMaterialData meshMat;
        MaterialHelper::extractMaterialData(scene, m, dir, meshMat, extractDir);
		auto mat = createMaterialFunc(meshObj, meshMat);
		if (!!mat) node->setMaterial(mat);

		_applyMeshNodeTransform(scene, node);

		return node;
	}

	auto node = _buildScene(scene, scene->mRootNode, SceneNodePtr(), filePath, createMaterialFunc, extractDir);
	node->setAttached(false); // root of object shouldnt be attached

							  // extract animations and add them one by one
							  // todo: use relative path from scene root (Nic)
	auto anims = Mesh::extractAnimations(scene, filePath);
	for (auto animName : anims.keys()) {
		auto anim = Animation::createFromSkeletalAnimation(anims[animName]);
		node->addAnimation(anim);
		// The FIRST clip, not the alphabetically last one — see above.
		if (node->getAnimations().size() == 1) node->setAnimation(anim);
	}

	node->applyDefaultPose();

	return node;
}

SceneNodePtr MeshNode::createDuplicate()
{
    auto node = MeshNode::create();

    // @todo: pass duplicates instead of copies!!!!!!!!
    node->setMesh(this->getMesh());
    node->meshPath = this->meshPath;
    node->meshIndex = this->meshIndex;
    node->setMaterial(this->material->duplicate());

	// todo: clone instead of copying (Nick)
	for (auto anim : animations) {
		node->addAnimation(anim);
	}
	node->setAnimation(animation);

    // Sockets are per-node authoring (see meshnode.h), so a duplicate carries
    // its own copy of the list — a second character has the same head socket
    // and does not share it.
    node->setSockets(this->sockets);

    return node;
}

bool MeshNode::hasBone(const QString &boneName) const
{
    if (skeleton.isNull() || boneName.isEmpty()) return false;
    return skeleton->boneMap.contains(boneName);
}

const Socket *MeshNode::findSocket(const QString &socketName) const
{
    for (const Socket &socket : sockets)
        if (socket.name == socketName) return &socket;
    return nullptr;
}

bool MeshNode::addSocket(const Socket &socket, QString *error)
{
    const auto refuse = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (socket.name.isEmpty()) return refuse(QStringLiteral("a socket needs a name"));
    if (findSocket(socket.name))
        return refuse(QStringLiteral("this node already has a socket named '%1'").arg(socket.name));
    if (skeleton.isNull() || skeleton->bones.isEmpty())
        return refuse(QStringLiteral("'%1' has no rig — sockets attach to BONES, so only a "
                                     "skinned mesh can carry one").arg(name));
    if (!hasBone(socket.boneName))
        return refuse(QStringLiteral("'%1' has no bone named '%2'").arg(name, socket.boneName));

    sockets.append(socket);
    return true;
}

bool MeshNode::removeSocket(const QString &socketName)
{
    for (int i = 0; i < sockets.size(); ++i) {
        if (sockets[i].name == socketName) {
            sockets.removeAt(i);
            return true;
        }
    }
    return false;
}

}
