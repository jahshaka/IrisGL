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

#include "core/math/quat.h"
#include "core/math/vec.h"
#include <QString>
#include <QColor>

#include "irisglfwd.h"
#include "document/animation/skeletalanimation.h"
#include "core/geometry/boundingsphere.h"
#include "core/geometry/aabb.h"
#include "document/assets/vertexlayout.h"
#include "document/assets/vertexbuffer.h"

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

    // glTF 2.0 metallic-roughness, read at import (GLB importer fix phase 0).
    // hasPbr is true when the source material carried any pbrMetallicRoughness
    // data; importers then build an iris::PbrMaterial from these instead of
    // faking a legacy Blinn-Phong material out of assimp's lossy
    // shininess back-conversion.
    bool hasPbr = false;
    QColor baseColorFactor = QColor(255, 255, 255, 255);
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    QString baseColorTexture;
    QString metallicTexture;    // split from the packed MR map (blue channel)
    QString roughnessTexture;   // split from the packed MR map (green channel)
    QString emissiveTexture;
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

            // Key times are stored in SECONDS (SKELETAL_PLAYBACK_SPEC S2):
            // assimp gives raw ticks + mTicksPerSecond (1000 for glTF/FBX,
            // 24/30 for many FBX/Collada exports; 0 means "unknown" — assimp's
            // documented convention is to assume 25). The old code stored raw
            // ticks and compensated with a `length > 60 → time × 1000` hack in
            // SceneNode::updateAnimation, which mis-played any non-ms clip.
            const double ticksPerSecond =
                anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0;

            // Clip names are kept and made unique (SKELETAL_PLAYBACK_SPEC S1).
            // The old code collapsed a clip named after its first channel to ""
            // — multiple clips then overwrote one QMap key, and saved
            // {source, name} references could never resolve on reload.
            auto animName = QString(anim->mName.C_Str());
            if (animName.isEmpty())
                animName = QString("clip %1").arg(i);
            const QString baseName = animName;
            for (int suffix = 2; anims.contains(animName); ++suffix)
                animName = baseName + QString(" %1").arg(suffix);

            auto skelAnim = SkeletalAnimation::create();
            skelAnim->name = animName;
            skelAnim->source = source;

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
