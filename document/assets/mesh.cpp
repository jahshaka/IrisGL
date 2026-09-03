/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/


#include <QQuaternion>
#include "document/assets/mesh.h"

#include <QString>
#include <QFile>
#include <QtMath>

#include "irisglfwd.h"
#include "core/logger.h"

#include "assimp/postprocess.h"
#include "import/importflags.h"
#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/mesh.h"


#include "document/assets/vertexbuffer.h"
#include "document/assets/vertexlayout.h"
#include "core/geometry/trimesh.h"
#include "document/assets/skeleton.h"
#include "core/math/trs.h"
#include "core/geometry/boundingsphere.h"
#include "core/geometry/aabb.h"

#include <functional>

namespace iris
{

QMatrix4x4 aiMatrixToQMatrix(aiMatrix4x4 aiMat) {
    aiVector3D pos,scale;
    aiQuaternion rot;

    aiMat.Decompose(scale,rot,pos);

    rot.Normalize();

    QMatrix4x4 mat;
    mat.setToIdentity();
    mat.translate(QVector3D(pos.x, pos.y, pos.z));
    mat.rotate(QQuaternion(rot.w, rot.x, rot.y, rot.z));
    mat.scale(QVector3D(scale.x, scale.y, scale.z));

    return mat;
}

Mesh::Mesh()
{
	triMesh = nullptr;
	numVerts = 0;
	usesIndexBuffer = false;
}

// http://ogldev.atspace.co.uk/www/tutorial38/tutorial38.html
Mesh::Mesh(aiMesh* mesh)
{
    triMesh = new TriMesh();

    this->vertexLayout = nullptr;
    numVerts = mesh->mNumFaces*3;
    numFaces = mesh->mNumFaces;

    if(!mesh->HasPositions())
        return;
        //throw QString("Mesh has no positions!!");

    this->addVertexArray(VertexAttribUsage::Position, (void*)mesh->mVertices, sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);


    if(mesh->HasTextureCoords(0))
        this->addVertexArray(VertexAttribUsage::TexCoord0, (void*)mesh->mTextureCoords[0], sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);
    if(mesh->HasTextureCoords(1))
        this->addVertexArray(VertexAttribUsage::TexCoord1, (void*)mesh->mTextureCoords[1], sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);
    if(mesh->HasNormals())
        this->addVertexArray(VertexAttribUsage::Normal, (void*)mesh->mNormals, sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);
    if(mesh->HasTangentsAndBitangents()) {
        this->addVertexArray(VertexAttribUsage::Tangent, (void*)mesh->mTangents, sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);
        // Bitangents carry the tangent-frame handedness (glTF TANGENT.w).
        // Without them the mirror cannot tell a mirrored UV island from a
        // regular one and authored tangent frames flip.
        this->addVertexArray(VertexAttribUsage::BiTangent, (void*)mesh->mBitangents, sizeof(aiVector3D) * mesh->mNumVertices, AttribTypeFloat,3);
    }

    if (mesh->HasBones()) {
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

        this->addVertexArray(VertexAttribUsage::BoneIndices, (void*)boneIndices.data(), sizeof(float) * boneIndices.size(), AttribTypeFloat, MAX_BONE_INDICES);
        this->addVertexArray(VertexAttribUsage::BoneWeights, (void*)boneWeights.data(), sizeof(float) * boneWeights.size(), AttribTypeFloat, MAX_BONE_INDICES);
    }

    // Assimp doesnt give the indices in an array
    // So some calculation still has to be done
    QVector<unsigned int> indices;
    indices.reserve(mesh->mNumFaces * 3);
    triMesh->triangles.reserve(mesh->mNumFaces);
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

        triMesh->addTriangle(QVector3D(a.x, a.y, a.z),
                             QVector3D(b.x, b.y, b.z),
                             QVector3D(c.x, c.y, c.z));
    }

    usesIndexBuffer = true;
    idxBuffer = IndexBuffer::create();
    idxBuffer->setData(indices.data(), sizeof(unsigned int) * indices.size());

    // the true size
    numVerts = indices.size();

    this->setPrimitiveMode(PrimitiveMode::Triangles);
	calculateBounds(mesh);
}

//todo: extract trimesh from data
Mesh::Mesh(void* data,int dataSize,int numElements,VertexLayout* vertexLayout)
{
    triMesh = nullptr;
    numVerts = numElements;

    auto vb = VertexBuffer::create(*vertexLayout);
    vb->setData(data, dataSize);
    vertexBuffers.append(vb);

    usesIndexBuffer = false;
    this->setPrimitiveMode(PrimitiveMode::Triangles);
}

void Mesh::setSkeleton(const SkeletonPtr &value)
{
    skeleton = value;
}

PrimitiveMode Mesh::getPrimitiveMode() const
{
    return primitiveMode;
}

void Mesh::setPrimitiveMode(const PrimitiveMode &value)
{
    primitiveMode = value;
}

void Mesh::clearVertexBuffers()
{
    this->vertexBuffers.clear();
}

void Mesh::addVertexBuffer(VertexBufferPtr vertexBuffer)
{
    this->vertexBuffers.append(vertexBuffer);
}

void Mesh::setIndexBuffer(IndexBufferPtr indexBuffer)
{
    this->idxBuffer = indexBuffer;
}

bool Mesh::hasSkeleton()
{
    return !!skeleton;
}

SkeletonPtr Mesh::getSkeleton()
{
    return skeleton;
}

void Mesh::addSkeletalAnimation(QString name, SkeletalAnimationPtr anim)
{
    skeletalAnimations.insert(name, anim);
}

QMap<QString, SkeletalAnimationPtr> Mesh::getSkeletalAnimations()
{
    return skeletalAnimations;
}

bool Mesh::hasSkeletalAnimations()
{
    return skeletalAnimations.count() != 0;
}

MeshPtr Mesh::loadMesh(QString filePath)
{
	// legacy -- update TODO
	Assimp::Importer importer;
	const aiScene *scene;

	QFile file(filePath);
	if (!file.exists())
	{
		irisLog("model " + filePath + " does not exists");
		return MeshPtr();
	}

	if (filePath.startsWith(":") || filePath.startsWith("qrc:")) {
		// loads mesh from resource
		if (!file.open(QIODevice::ReadOnly))
		    qWarning("Mesh::loadMesh: failed to open %s", qUtf8Printable(filePath));
		auto data = file.readAll();
		scene = importer.ReadFileFromMemory((void*)data.data(),
			data.length(),
			iris::ImportFlags::Canonical);
	}
	else {
		// load mesh from file
		scene = importer.ReadFile(filePath.toStdString().c_str(),
			iris::ImportFlags::Canonical);
	}

	if (!scene) {
		irisLog("model " + filePath + ": error parsing file");
		return MeshPtr();
	}

	if (scene->mNumMeshes <= 0) {
		irisLog("model " + filePath + ": scene has no meshes");
		return MeshPtr();
	}

	auto mesh = scene->mMeshes[0];
	auto meshObj = new Mesh(scene->mMeshes[0]);
	auto skel = extractSkeleton(mesh, scene);

	if (!!skel)
		meshObj->setSkeleton(skel);

	auto anims = extractAnimations(scene);
	for (auto animName : anims.keys())
	{
		meshObj->addSkeletalAnimation(animName, anims[animName]);
	}

	return MeshPtr(meshObj);
}

MeshPtr Mesh::loadAnimatedMesh(QString filePath)
{
    Assimp::Importer importer;
    const aiScene *scene;

    if (filePath.startsWith(":") || filePath.startsWith("qrc:")) {
        // loads mesh from resource
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly))
            qWarning("Mesh::loadAnimatedMesh: failed to open %s", qUtf8Printable(filePath));
        auto data = file.readAll();
        scene = importer.ReadFileFromMemory((void*)data.data(),
                                            data.length(),
                                            iris::ImportFlags::Canonical);
    } else {
        scene = importer.ReadFile(filePath.toStdString().c_str(),
                                  iris::ImportFlags::Canonical);
    }

    //extract animations from scene
    auto mesh = new Mesh(scene->mMeshes[0]);

    return MeshPtr(mesh);
}

SkeletonPtr Mesh::extractSkeleton(const aiMesh *mesh, const aiScene *scene)
{
    if (mesh->mNumBones == 0)
        return SkeletonPtr();

    // create skeleton and add animations
    auto skel = Skeleton::create();
    for (unsigned i = 0;i<mesh->mNumBones; i++) {
        auto meshBone = mesh->mBones[i];

        auto bone = Bone::create(QString(meshBone->mName.C_Str()));
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
        auto bone = skel->getBone(QString(node->mName.C_Str()));
        // A bone may appear twice in a node tree only if the file is malformed;
        // don't re-parent one that already has a parent.
        if (!!bone && !!ancestor && !bone->parentBone && bone != ancestor)
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
        const QMatrix4x4 bindLocal = !bone->parentBone.isNull()
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

Mesh* Mesh::create(void* data,int dataSize,int numVerts,VertexLayout* vertexLayout)
{
    return new Mesh(data,dataSize,numVerts,vertexLayout);
}

MeshPtr Mesh::create()
{
	auto mesh = new Mesh();
	return MeshPtr(mesh);
}

Mesh::~Mesh()
{
    //delete vertexLayout;
	if (triMesh)
		delete triMesh;
}

void Mesh::setVertexCount(const unsigned int count)
{
	numVerts = count;
}

void Mesh::addVertexArray(VertexAttribUsage usage,void* dataPtr,int size,int type,int numComponents)
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
    vertexBuffers.append(vb);
}

void Mesh::calculateBounds(const aiMesh* mesh)
{
	aabb = AABB();
	for (unsigned int i = 0; i<mesh->mNumVertices; i++) {
		auto& vert = mesh->mVertices[i];
		aabb.merge(QVector3D(vert.x, vert.y, vert.z));
	}

	boundingSphere = aabb.getMinimalEnclosingSphere();
}

// https://github.com/playcanvas/engine/blob/master/src/shape/bounding-sphere.js#L30
// bounding sphere wont necessarily be at the center of the mesh's origin
// the true positon in world space would be the bounds's center plus the mesh's absolute position
BoundingSphere Mesh::calculateBoundingSphere(const aiMesh *mesh)
{
    // find average pos
    aiVector3D averagePos;// = mesh->mVertices[0];
    aiVector3D sum(0,0,0);

    for(unsigned int i =0;i<mesh->mNumVertices;i++) {
        sum += mesh->mVertices[i];

        if (i%100 == 0) {
            sum /= mesh->mNumVertices;
            averagePos += sum;
            sum = aiVector3D(0,0,0);
        }
    }

    sum/=mesh->mNumVertices;
    averagePos += sum;

    //averagePos = averagePos/mesh->mNumVertices;

    // find furthest distance
    float maxDistSqrd = 0;
    for(unsigned int i =0;i<mesh->mNumVertices;i++) {
        auto vert = mesh->mVertices[i];
        auto diff = vert-averagePos;
        auto dist = diff.SquareLength();

        if (dist > maxDistSqrd)
            maxDistSqrd = dist;
    }

    BoundingSphere sphere;
    sphere.pos = QVector3D(averagePos.x, averagePos.y, averagePos.x);
    sphere.radius = qSqrt(maxDistSqrd);
    return sphere;
}

}
