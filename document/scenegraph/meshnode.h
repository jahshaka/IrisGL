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
#include "document/scenegraph/socket.h"
#include "core/irisutils.h"
#include "document/assets/texture2d.h"
#include "document/assets/mesh.h"

// No assimp in this header (ENGINEERING_DEBT L4 part 3, 2026-09-10): assimp
// is a PRIVATE dependency of IrisGL. The parse a caller may keep is the
// opaque SceneSource (import/scenesource.h); the importer's progress hook is
// an implementation detail of meshnode.cpp.
#include "import/scenesource.h"

struct aiScene;

namespace iris
{

struct MeshMaterialData;

class IModelReadProgress
{
public:
    virtual float onProgress(float percentage) = 0;
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

    FaceCullingMode faceCullingMode;

    /// THE SCENE'S DEFAULT FLOOR (owner, 2026-09-12). Marks the node Studio's
    /// default-floor factory built — the one whose OWN default material a
    /// "reset material" restores (Studio services/defaultfloor.h). A document
    /// flag, serialized as `defaultFloor` and reflected as the "defaultFloor"
    /// property, so it survives save/reopen and archives and is never
    /// inferred from a name or a mesh path (`isBuiltIn` cannot carry it: it is
    /// set on many runtime-only nodes and is not serialized). NOT copied by
    /// createDuplicate: a scene has one default floor, and a copy is a mesh.
    bool defaultFloor = false;

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

    /// The fragment from a parse a caller already holds (the threaded open's
    /// prewarm, import's one-parse rule): no file access at all.
    static SceneNodePtr loadAsSceneFragment(
        const QString &filePath,
        const SceneSource &source,
        std::function<MaterialPtr(MeshPtr mesh, MeshMaterialData& data)> createMaterialFunc,
        const QString &extractDir = QString()
    );

    /// IrisGL-internal form of the above (a complete aiScene needs assimp's
    /// headers, which only this library's own translation units and the
    /// white-box importer suites include).
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
    /// Whether this node's rig has a bone by this name.
    bool hasBone(const QString &boneName) const;

    // ---- sockets (CAMERAS_SPEC §5, D9; see scenegraph/socket.h) -----------
    //
    // Named attach points on this node's BONES. They live on the NODE and not
    // on the mesh asset for the same reason the pose does: two characters
    // sharing one rig must be able to carry different sockets, and a socket is
    // authored per character, not per file.

    /// This node's sockets, in the order they were added (the order the file
    /// writes and `node.sockets` reports).
    const QList<Socket> &getSockets() const { return sockets; }
    /// The socket with this name, or null. The pointer is into `sockets` and is
    /// invalidated by any add/remove — read it, do not keep it.
    const Socket *findSocket(const QString &name) const;
    /// Adds a socket. Refused (false, with `error` set when given) for an empty
    /// name, a name this node already uses, or a bone this node's rig does not
    /// have — including "this node has no rig at all", which is the refusal a
    /// caller most often means to hit.
    bool addSocket(const Socket &socket, QString *error = nullptr);
    /// Removes the socket by name. False when there was none.
    /// NOTE: this does NOT detach whatever was riding it — that is the scene's
    /// job (Scene::detachFromSocket), because a node knows nothing about who
    /// points at it. An attachment left pointing at a removed socket simply
    /// stops moving (the fail-soft rule in socket.h).
    bool removeSocket(const QString &name);
    /// Replaces the whole list (the reader, and duplication).
    void setSockets(const QList<Socket> &list) { sockets = list; }

    void setMaterial(MaterialPtr material);

    MaterialPtr getMaterial() {
        return material;
    }

    // not needed because this guy likes public members...
    // shouldnt be here at all, the value is already set in the constructor...
    void setNodeType(SceneNodeType type) {
        sceneNodeType = type;
    }

    SceneNodePtr createDuplicate() override;

    /// A SKINNED mesh is never static (SCENEGRAPH_SPEC §6): its Item carries a
    /// SkeletonInstance that the engine poses every frame, and a static Item is
    /// not in the per-frame bounds list at all — the character would render at
    /// its bind-pose bounds and be culled wrong. Sockets ride skinned nodes too.
    bool isStaticEligible() const override
    {
        return !skeleton && SceneNode::isStaticEligible();
    }

    /// MOBILITY rule 1 for a mesh: a rig WITH A CLIP moves
    /// (REALTIME_REFLECTIONS_SPEC §3.3.2). A rigged mesh with no clip attached
    /// is not moving anything — it renders at its rest pose — so it resolves
    /// static and is reflected and voxelised like any other prop. (It still
    /// cannot be GRAPH-static, see isStaticEligible above: that is the engine's
    /// memory-manager constraint, not a statement about movement.)
    ///
    /// The base class already answers `skeleton` for a clip carried on an
    /// Animation; this adds the clips the MESH ASSET carries, which is the
    /// shape an imported character arrives in.
    bool hasMobilityDriver(MobilityReason *why = nullptr) const override
    {
        if (SceneNode::hasMobilityDriver(why)) return true;
        if (!skeleton.isNull() && !mesh.isNull() && mesh->hasSkeletalAnimations()) {
            if (why) *why = MobilityReason::Skeleton;
            return true;
        }
        return false;
    }

    float getMeshRadius();
    BoundingSphere getTransformedBoundingSphere();

    FaceCullingMode getFaceCullingMode() const;
    void setFaceCullingMode(const FaceCullingMode &value);

private:
    MeshNode();
    /// Clones the mesh's rig template into this node's `skeleton` (or clears it).
    void adoptSkeletonFromMesh();

    QList<Socket> sockets;
};

}

#endif // MESHNODE_H
