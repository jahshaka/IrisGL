/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef MESHNODE_H
#define MESHNODE_H

#include <functional>
#include "irisglfwd.h"
#include "document/scenegraph/scenenode.h"
#include "core/irisutils.h"
#include "document/assets/texture2d.h"
#include "document/assets/mesh.h"

#include "assimp/Importer.hpp"
#include "assimp/ProgressHandler.hpp"

class aiScene;

namespace iris
{

struct MeshMaterialData;

class IModelReadProgress
{
public:
    virtual float onProgress(float percentage) = 0;
};

class ModelProgressHandler : public Assimp::ProgressHandler
{
public:
    IModelReadProgress *handler;
    ModelProgressHandler() : ProgressHandler() {
        handler = Q_NULLPTR;
    }

    void setHandler(IModelReadProgress* handler) {
        this->handler = handler;
    }

    ~ModelProgressHandler() {

    }

    bool Update(float percentage) {
        if (handler) handler->onProgress(percentage);
        return 1;
    }
};

class SceneSource
{
public:
    SceneSource() = default;
    Assimp::Importer importer;
};

enum class FaceCullingMode
{
	None,
	Front,
	Back,
	DefinedInMaterial
};

class MeshNode : public SceneNode
{
public:
    MeshPtr mesh;

    QString meshPath;

    /**
     * This holds the index of the mesh
     */
    int meshIndex;

    MaterialPtr material;
    MaterialPtr customMaterial;

    FaceCullingMode faceCullingMode;

    // For animated meshes, the rootBone's transform is what will be used as its transform
    // Since all its animations are based at the rootBone
    SceneNodePtr rootBone;

    /// Per-node pose (see getSkeleton()). Derived state: cloned from the mesh's
    /// rig template in setMesh, never serialized (meshes and skeletons are
    /// written by reference — no file-format change, no migration).
    SkeletonPtr skeleton;

    static MeshNodePtr create() {
        return MeshNodePtr(new MeshNode());
    }

    virtual QList<Property*> getProperties() override;
    virtual QVariant getPropertyValue(QString valueName) override;
    virtual bool setPropertyValue(QString valueName, const QVariant &value) override;

    /**
     * Some model contains multiple meshes with child-parent relationships. This funtion Loads the model as a scene
     * itself. If only one mesh is in the scene, it returns it as an MeshNode rather than a SceneNode. Otherwise, it
     * returns a null shared pointer.
     * @param path
     * @return
     */
    /// `scene_` may be null: a local importer is used and the aiScene dies
    /// with the call (everything returned owns copies). Pass a SceneSource
    /// only to keep the aiScene alive for follow-up work (metadata counts).
    /// `extractDir`: where embedded textures and derived maps are WRITTEN
    /// (import staging). Empty = beside the source file (legacy behavior —
    /// wrong for read-only sources; the import pipeline always passes one).
    static SceneNodePtr loadAsSceneFragment(
        QString path,
        std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
        SceneSource *scene_ = Q_NULLPTR,
        IModelReadProgress* progressReader = Q_NULLPTR,
        const QString &extractDir = QString()
    );

	static SceneNodePtr loadAsSceneFragment(
		const QString &filePath,
		const aiScene* scene_,
		std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
		const QString &extractDir = QString()
	);

    static SceneNodePtr loadAsAnimatedModel(QString path);

    void setMesh(QString source);
    void setMesh(MeshPtr mesh);

    MeshPtr getMesh();

    /// This NODE's pose (GPU_SKINNING_SPEC §7). The `SkeletonPtr` on the
    /// iris::Mesh is the rig TEMPLATE — shared by every node that references
    /// the mesh asset and never posed. setMesh() clones it here, so each
    /// MeshNode (including every createDuplicate) owns its own bone transforms
    /// and duplicates of one rig animate independently.
    /// Null when the mesh has no skeleton.
    SkeletonPtr getSkeleton() const { return skeleton; }
    bool hasSkeleton() const { return !skeleton.isNull(); }

    void setMaterial(MaterialPtr material);

    MaterialPtr getMaterial() {
        return material;
    }

    MaterialPtr getCustomMaterial() {
        return customMaterial;
    }

    // not needed because this guy likes public members...
    // shouldnt be here at all, the value is already set in the constructor...
    void setNodeType(SceneNodeType type) {
        sceneNodeType = type;
    }

    SceneNodePtr createDuplicate() override;
    float getMeshRadius();
    BoundingSphere getTransformedBoundingSphere();

    FaceCullingMode getFaceCullingMode() const;
    void setFaceCullingMode(const FaceCullingMode &value);

private:
    MeshNode();
    /// Clones the mesh's rig template into this node's `skeleton` (or clears it).
    void adoptSkeletonFromMesh();
};

}

#endif // MESHNODE_H
