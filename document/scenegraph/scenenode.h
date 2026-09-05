/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef SCENENODE_H
#define SCENENODE_H


#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "irisglfwd.h"
#include "document/physics/physicsproperties.h"

namespace iris
{

enum class SceneNodeType {
    Empty,
    ParticleSystem,
    Mesh,
    Light,
    Camera,
    Viewer,
    // A projected-texture decal (DECALS_SPEC). Every node type's constructor
    // SETS its own value here and every type switch in both repos depends on
    // it. (CameraNode was the one that never did, for the whole life of the
    // codebase — so cameras read as Empty everywhere. Fixed by CAMERAS_SPEC
    // phase 1, together with a sweep of every switch it re-routes.)
    Decal
};

class PhysicsProperty;
class Property;
class Animation;
class PropertyAnim;
typedef QSharedPointer<Animation> AnimationPtr;

class SceneNode : public QEnableSharedFromThis<SceneNode>
{
protected:
    QList<AnimationPtr> animations;
    AnimationPtr animation;

    iris::Vec3 pos;
    iris::Vec3 scale;
    iris::Quat rot;

    // ---- transform invalidation (deep audit 2026-09, area 3) --------------
    // These two flags were set true in the constructor and NEVER cleared
    // anywhere in the tree, so update() recomposed localTransform AND
    // globalTransform for every node of every scene, every frame — a cache
    // that only ever cost. They mean what they say now:
    //
    //   transformDirty    this node's own TRS changed, so localTransform must
    //                     be recomposed (and globalTransform with it). Set by
    //                     every mutator; see setTransformDirty().
    //   globalDirty       an ANCESTOR moved, so only globalTransform needs
    //                     recomputing. This is the DOWNWARD half of the
    //                     invalidation and update() is what sets it, because
    //                     setTransformDirty walks UPWARDS (it tells ancestors
    //                     to descend) and can never reach a subtree.
    //   hasDirtyChildren  some descendant needs update() work at all; the flag
    //                     that lets an untouched branch be skipped whole.
    //
    // All three are cleared at the end of update(). A mutator that forgets to
    // set them is a stale-transform bug — the picker, the gizmos and the
    // mirror's selection outline all read `globalTransform` directly.
    bool transformDirty;
    bool globalDirty;
    bool hasDirtyChildren;
public:
    // cached local and global transform
    iris::Mat4 localTransform;
    iris::Mat4 globalTransform;

    SceneNodeType sceneNodeType;

    QString name;
    /// Process-unique, monotonic, and 64 bits WHATEVER the platform: this was
    /// `long`, which is 32 bits on Windows LLP64 (deep audit 2026-09, area 5).
    /// SceneMirror keys its entry map on it, so a wrap is a silent aliasing of
    /// two different nodes onto one engine entry.
    qint64 nodeId;

    // OWNERSHIP (deep audit 2026-09, area 3 / List B item 4). The tree has ONE
    // ownership direction: a parent owns its children, strongly. The two
    // back-references are therefore WEAK — they were QSharedPointer until this
    // change, which made every parent/child pair and every node/scene pair a
    // reference cycle, so nothing in a document ever died.
    //
    // Read them through getScene() / getParent(), never directly: a weak
    // pointer must be locked before it is used, and the lock is what keeps the
    // target alive for the duration of the expression. The two members stay
    // public only because the codebase is full of `node->parent` call sites
    // that now read as `node->getParent()`; new code has no reason to touch
    // them.
    SceneWPtr scene;
    SceneNodeWPtr parent;
    QList<SceneNodePtr> children;

    // editor specific
    bool duplicable;
	bool exportable;
    bool visible;
    bool removable;
    bool isBuiltIn;
	bool isPhysicsBody;

	// Prevents physics body from updating the transform of
	// the node.
	bool disablePhysicsTransform = false;

    // ---- socket attachment (CAMERAS_SPEC §5; see scenegraph/socket.h) -----
    //
    // "This node rides bone X of that character." Both fields are empty for the
    // overwhelming majority of nodes; when they are set, SocketResolver writes
    // this node's transform every frame from the owner's posed bone, and any
    // transform the user or a script sets is overwritten on the next frame
    // (which is what "attached" means — detach first to move it by hand).
    //
    // The owner is named by GUID and not by pointer on purpose: it survives the
    // file, it survives a re-import that rebuilds the node objects, and a
    // dangling one is inert rather than a crash.
    QString socketOwnerGuid;
    QString socketName;

    bool isSocketAttached() const { return !socketOwnerGuid.isEmpty() && !socketName.isEmpty(); }
    /// The RAW setter — no validation, no registry. The reader and duplication
    /// use it; everything else goes through Scene::attachToSocket, which
    /// validates the owner and keeps the scene's attachment registry correct.
    void setSocketAttachment(const QString &ownerGuid, const QString &socket)
    {
        socketOwnerGuid = ownerGuid;
        socketName = socket;
    }

	// Bullet interpolates the transform of physics bodies
	// to give them a smooth movement. This bool disables that
	// and uses the actual transform of the body.
	bool useInterpolatedPhysicsTransform = true;

    PhysicsProperty physicsProperty;

    bool pickable;
	uint64_t pickingGroups;
    bool castShadow;

    // PLANAR REFLECTIONS (PLANAR_REFLECTIONS_SPEC.md §7 option A): this node's
    // flat surface is a mirror plane. Deliberately a FLAG on an ordinary mesh
    // node rather than a MirrorNode scene-node type — the plane, its size and
    // its normal are all derived from the mesh's own bounds. (The other half
    // of that reasoning — "SceneNodeType is unreliable, CameraNode never sets
    // its own" — no longer holds: CAMERAS_SPEC phase 1 fixed it. The flag
    // stays a flag for the geometric reason above.) Off by default: an active
    // reflector is a whole extra scene render and cannot be inferred.
    bool planarReflector = false;

	mutable QString guid;

    friend class Renderer;
    friend class Scene;

    // If a node is attached to parents then it inherits animations
    // It also cant have its own animation
    // Attached nodes are usually nodes created from model files
    // The scenenode is a part of the heirarchy of the model's structure
    // so its attached to the root node of the model
    bool attached;

public:
    SceneNode();
	SceneNode(const SceneNode&);
    virtual ~SceneNode() {}

    static SceneNodePtr create();

    void setName(QString name);
    QString getName();

    qint64 getNodeId();

	void setGUID(const QString &id) const {
		guid = id;
	}

	const QString getGUID() const {
		return guid;
	}

    bool isDuplicable() {
        return duplicable;
    }

	bool isExportable() {
		return exportable;
	}

    iris::Vec3 getLocalPos() {
        return pos;
    }

    iris::Quat getLocalRot() {
        return rot;
    }

    iris::Vec3 getLocalScale() {
        return scale;
    }

	void rotate(iris::Quat rot, bool global = false);
    
    bool hasChildren() {
        return !children.empty();
    }

    void setLocalPos(iris::Vec3 pos);
    void setLocalRot(iris::Quat rot);
    void setLocalScale(iris::Vec3 scale);

    void setLocalTransform(iris::Mat4 transformMatrix);

    void setTransformDirty();
    void setHasDirtyChildren();

    bool isAttached();
    void setAttached(bool attached);

	/// The owning parent, locked. Null for a root node — and also null for a
	/// node whose parent has been destroyed, which is a state that could not
	/// exist while `parent` was strong.
	SceneNodePtr getParent() const
	{
		return parent.lock();
	}

	/// The scene this node belongs to, locked. Null when the node is detached,
	/// and null once the scene itself is gone.
	ScenePtr getScene() const
	{
		return scene.lock();
	}

	/// "Is there a live parent / scene", without materialising a
	/// QSharedPointer. QWeakPointer::isNull() already reports expiry — it turns
	/// true the moment the last strong reference goes — so these are the honest
	/// replacements for the old `!!node->parent` / `!!node->scene` tests.
	bool hasParent() const { return !parent.isNull(); }
	bool hasScene()  const { return !scene.isNull(); }

    void addAnimation(AnimationPtr anim);
    QList<AnimationPtr> getAnimations();
    void setAnimation(AnimationPtr anim);
    AnimationPtr getAnimation();
    bool hasActiveAnimation();
    void deleteAnimation(int index);
    void deleteAnimation(AnimationPtr anim);

    virtual QList<Property*> getProperties();
    virtual QVariant getPropertyValue(QString valueName);
    /// Mirror of getPropertyValue: applies a value by property name. Returns
    /// false when the node has no such property (callers report, not crash).
    /// Base handles position/rotation (euler degrees)/scale; subclasses extend
    /// exactly the set their getProperties() advertises.
    virtual bool setPropertyValue(QString valueName, const QVariant &value);

    /*
    * This function should return an exact copy of this node
    * with a few exceptions:
    * 1) The duplicate shouldnt have a parent node or be added to a scene
    */
   virtual SceneNodePtr createDuplicate() {
       //qt_assert((QString("This node isnt duplicable: ") + name).toStdString().c_str(),__FILE__,__LINE__);
	   return SceneNodePtr(new SceneNode());
   }

   SceneNodePtr duplicate();

private:
   /// duplicate()'s recursion. `guidMap` records old guid -> new guid for the
   /// WHOLE copied subtree, which the public duplicate() then uses to re-point
   /// socket attachments: copying a character with a camera on its head must
   /// give the COPY's camera the COPY's head, not the original's (the guids are
   /// regenerated, so a straight field copy would have every duplicate riding
   /// the first character forever).
   SceneNodePtr duplicateInto(QHash<QString, QString> &guidMap);
   /// Re-points this subtree's socket owners through `guidMap`; an owner
   /// OUTSIDE the copied subtree keeps its guid, which is the "second camera on
   /// the same character" case and is correct.
   void remapSocketOwners(const QHash<QString, QString> &guidMap);
public:

    bool isVisible() {
        return visible;
    }

	void setVisible(bool flag = true) {
		visible = flag;
	}

    void show(bool hideChildren = false) {
        visible = true;
		if (hideChildren) {
			for (const auto &child : children)
				child->show(hideChildren);
		}
    }

    void hide(bool hideChildren = false) {
        visible = false;
		if (hideChildren) {
			for (const auto &child : children)
				child->hide(hideChildren);
		}
    }

    bool isRemovable() {
        return removable;
    }

    void setPickable(bool canPick) {
        pickable = canPick;
    }

    bool isPickable() {
        return pickable;
    }

    void setShadowCastingEnabled(bool val) {
        castShadow = val;
    }

    bool getShadowCastingEnabled() {
        return castShadow;
    }

    void setPlanarReflector(bool val) {
        planarReflector = val;
    }

    bool getPlanarReflector() const {
        return planarReflector;
    }

    SceneNodeType getSceneNodeType();
    /**
     * @brief addChild
     * @param node
     * @param keepTransform keeps visual transform
     */
    void addChild(SceneNodePtr node, bool keepTransform = true);
    void insertChild(int position, SceneNodePtr node, bool keepTransform = true);
    void removeFromParent();
    void removeChild(SceneNodePtr node);

    bool isRootNode();

	iris::Quat getGlobalRotation();
    iris::Vec3 getGlobalPosition();
    iris::Mat4 getGlobalTransform();
    iris::Mat4 getLocalTransform();

	void setGlobalPos(iris::Vec3 pos);
	void setGlobalRot(iris::Quat rot);
	void setGlobalTransform(iris::Mat4 transform);

    /*
     * This function does multiple things:
     * - Calculates the transformation of the objects
     * - Particle systems use this to update animations
     */
    virtual void update(float dt);
    /// Property animations only — position / rotation / scale on THIS node.
    /// Skeletal clips are the engine's since the clip evaluator was retired
    /// (ANIMATION_ENGINE_MIGRATION_SPEC); the document keeps the authored data
    /// and the clock, and Scene::animationTime is what the mirror pushes.
    virtual void updateAnimation(float time);
    /// Snapshots the subtree's authored local transforms. Named for what it
    /// used to do (compose the rest pose into skin matrices); it is now the
    /// rest-pose capture that clip translation reads.
    void applyDefaultPose();

    /// The node's authored local transform, snapshotted while the subtree was
    /// known to be at REST — i.e. before any clip had posed it.
    ///
    /// Clip translation needs it (ANIMATION_ENGINE_MIGRATION_SPEC §3.1 step 3):
    /// composing a bone's chain at time t evaluates each node on that chain with
    /// its CLIP key if it has one and its AUTHORED transform otherwise, and
    /// "authored" is not the same as "current" once a different clip has run —
    /// which is precisely why the Avatar page had to snapshot and restore a rest
    /// pose around every clip switch.
    ///
    /// Captured by applyDefaultPose(), whose four call sites are exactly the
    /// four moments a fragment is known to be at rest (scene load and fragment
    /// import). `hasRest` is false for a node nobody ever captured, and callers
    /// then fall back to the live transform.
    bool        hasRest = false;
    iris::Vec3   restPos;
    iris::Quat restRot;
    iris::Vec3   restScale{1, 1, 1};

private:
    void setParent(SceneNodePtr node);
    void setScene(ScenePtr scene);
    void removeFromScene();

    static qint64 generateNodeId();
};

}

Q_DECLARE_METATYPE(iris::SceneNode)
Q_DECLARE_METATYPE(iris::SceneNodePtr)
#endif // SCENENODE_H
