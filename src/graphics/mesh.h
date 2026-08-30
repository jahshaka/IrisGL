/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef MESH_H
#define MESH_H

#include <QQuaternion>
#include <QString>
#include <QColor>

#include "../irisglfwd.h"
#include "../animation/skeletalanimation.h"
#include "../geometry/boundingsphere.h"
#include "../geometry/aabb.h"
#include "vertexlayout.h"
#include "vertexbuffer.h"

#include "assimp/scene.h"

class aiMesh;

namespace iris
{

class BoundingSphere;

struct MeshMaterialData
{
    QColor diffuseColor;
    QColor specularColor;
    QColor ambientColor;
    QColor emissionColor;
    float shininess;

    QString diffuseTexture;
    QString specularTexture;
    QString normalTexture;
    QString hightTexture;

	QString nodeName;

    bool hasEmbeddedDiffTexture = false;
    bool hasEmbeddedSpecularTexture = false;
    bool hasEmbeddedNormalTexture = false;
    bool hasEmbeddedHightTexture = false;
};

enum class PrimitiveMode
{
    Triangles,
    Lines,
    LineLoop,
	LineStrip
};

// CPU-side mesh: geometry buffers, skeleton, animations, bounds and the picking
// TriMesh. The GL half (VAO/draw) died with the legacy renderer at step 14; the
// engine mirror converts these buffers into engine meshes each time one changes.
class Mesh
{
    SkeletonPtr skeleton;
    QMap<QString, SkeletalAnimationPtr> skeletalAnimations;

	QList<VertexBufferPtr> vertexBuffers;
	IndexBufferPtr idxBuffer;

public:
    PrimitiveMode primitiveMode;
    bool usesIndexBuffer;

    BoundingSphere boundingSphere;
	AABB aabb;

    VertexLayout* vertexLayout;
    int numVerts;
    int numFaces;

    TriMesh* triMesh;
    /// CPU-side geometry, read-only. The engine mirror and importers convert from
    /// these.
    const QList<VertexBufferPtr>& getVertexBuffers() const { return vertexBuffers; }
    IndexBufferPtr getIndexBuffer() const { return idxBuffer; }

    TriMesh* getTriMesh()
    {
        return triMesh;
    }


    bool hasSkeleton();
    SkeletonPtr getSkeleton();

    void addSkeletalAnimation(QString name, SkeletalAnimationPtr anim);
    QMap<QString, SkeletalAnimationPtr> getSkeletalAnimations();
    bool hasSkeletalAnimations();

    static MeshPtr loadMesh(QString filePath);
    static MeshPtr loadAnimatedMesh(QString filePath);
    static SkeletonPtr extractSkeleton(const aiMesh* mesh, const aiScene* scene);
    static QMap<QString, SkeletalAnimationPtr> extractAnimations(const aiScene *scene, QString source = "")
    {
        QMap<QString, SkeletalAnimationPtr> anims;

        for (unsigned i = 0; i<scene->mNumAnimations; i++) {
            auto anim = scene->mAnimations[i];
            auto animName = QString(anim->mName.C_Str());
            if (!anim || animName == anim->mChannels[0]->mNodeName.C_Str()) {
                animName = "";
            }

            auto skelAnim = SkeletalAnimation::create();
            skelAnim->name = animName;
            skelAnim->source = source;

            for (unsigned j = 0; j<anim->mNumChannels; j++) {
                auto nodeAnim = anim->mChannels[j];

                auto nodeName = QString(nodeAnim->mNodeName.C_Str());
                auto boneAnim = new BoneAnimation();

                // extract tracks
                for (unsigned k = 0; k<nodeAnim->mNumPositionKeys; k++) {
                    auto key = nodeAnim->mPositionKeys[k];
                    boneAnim->posKeys->addKey(QVector3D(key.mValue.x, key.mValue.y, key.mValue.z), key.mTime);
                }

                for (unsigned k = 0; k<nodeAnim->mNumRotationKeys; k++) {
                    auto key = nodeAnim->mRotationKeys[k];
                    boneAnim->rotKeys->addKey(QQuaternion(key.mValue.w, key.mValue.x, key.mValue.y, key.mValue.z), key.mTime);
                }

                for (unsigned k = 0; k<nodeAnim->mNumScalingKeys; k++) {
                    auto key = nodeAnim->mScalingKeys[k];
                    boneAnim->scaleKeys->addKey(QVector3D(key.mValue.x, key.mValue.y, key.mValue.z), key.mTime);
                }

                skelAnim->addBoneAnimation(nodeName, boneAnim);
            }

            anims.insert(animName, skelAnim);
        }

        return anims;
    }


    //assumed ownership of vertexLayout
    static Mesh* create(void* data,int dataSize,int numElements,VertexLayout* vertexLayout);
	static MeshPtr create(VertexLayout vertexLayout);
	static MeshPtr create();

	Mesh();

    Mesh(aiMesh* mesh);

    /**
     *
     * @param data
     * @param dataSize
     * @param numElements number of vertices
     * @param vertexLayout
     */
    Mesh(void* data,int dataSize,int numElements,VertexLayout* vertexLayout);

    ~Mesh();

	void setVertexCount(const unsigned int count);
    void setSkeleton(const SkeletonPtr &value);

    PrimitiveMode getPrimitiveMode() const;
    void setPrimitiveMode(const PrimitiveMode &value);

    void clearVertexBuffers();
	void addVertexBuffer(VertexBufferPtr vertexBuffer);
	void setIndexBuffer(IndexBufferPtr indexBuffer);

	AABB getAABB(){return aabb;}
	BoundingSphere getBoundingSphere() { return boundingSphere; }

private:
    void addVertexArray(VertexAttribUsage usage,void* data,int size,int type,int numComponents);

	void calculateBounds(const aiMesh* mesh);

    static BoundingSphere calculateBoundingSphere(const aiMesh* mesh);
};

}

#endif // MESH_H
