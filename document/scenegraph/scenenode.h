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
#include "document/scenegraph/nodegraph.h"

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

/// What a mutation changed. The funnel below reports it; v1.5's undo capture
/// and the properties panel's live refresh are meant to ride this seam rather
/// than polling (SPECS/SCENEGRAPH_SPEC.md §1: "the handle notifies; Ogre has no
/// write events — its only transform hook is compiled out in release").
enum class NodeChange {
    Transform,   ///< local or world TRS
    Structure,   ///< parent / child list / scene membership
    Visibility,
    Flags,       ///< pickable, castShadow, planarReflector, ...
    Name
};

// -----------------------------------------------------------------------------
// iris::SceneNode — a TYPED HANDLE onto one Ogre::SceneNode.
//
// SPECS/SCENEGRAPH_SPEC.md D2. This class kept its name and its whole public
// API on purpose (the audit counted ~620 transform and ~548 hierarchy call
// sites), but it no longer OWNS any of the graph. Gone, by construction:
//
//   pos / rot / scale            -> Ogre's SoA transform
//   localTransform, globalTransform (the two cached Mat4, 128 bytes a node)
//   transformDirty / globalDirty / hasDirtyChildren
//   update()'s transform recursion
//   getGlobalTransform()'s recompute-and-write-the-cache body (audit F2 — the
//     non-const "getter" that walked and wrote the whole ancestor chain; it is
//     now Ogre's own dirty-gated _getFullTransformUpdated())
//   QList<SceneNodePtr> children as the HIERARCHY
//
// What the handle still owns is everything that was never graph structure:
// identity (guid + nodeId), the editor flags, physics, animation clips, the
// rest pose, asset references and the properties/reflection surface.
//
// OWNERSHIP, and why one list survives. Ogre's node holds a RAW back-pointer to
// this handle (UserObjectBindings), never a QSharedPointer: an Ogre-held strong
// reference would make `graph::destroyNode` the only way a node could ever die,
// and the undo stack's contract is the opposite — "the undo stack is what keeps
// deleted nodes alive" (audit §3.3). So `mChildRefs` below is kept as a pure
// LIFETIME ANCHOR: it holds the QSharedPointers, it is never read for order,
// parentage or iteration, and exactly two functions (insertChild / removeChild)
// touch it. Structure comes from Ogre, always.
// -----------------------------------------------------------------------------
class SceneNode : public QEnableSharedFromThis<SceneNode>
{
protected:
    QList<AnimationPtr> animations;
    AnimationPtr animation;

    /// This node's Ogre::SceneNode, opaque here (see nodegraph.h). Null only
    /// when no Ogre::Root existed at construction time — a v1 programming
    /// error, reported once and loudly, not a supported mode.
    graph::NodeHandle mGraphNode = nullptr;

    /// LIFETIME ANCHOR ONLY — see the class note. Never iterate it; use
    /// children().
    QList<SceneNodePtr> mChildRefs;

    /// How many times mGraphNode has been REPLACED. See graphEpoch().
    quint32 mGraphEpoch = 0;

    /// SCENE_STATIC, the document's side of it. See setStaticHint().
    bool mStaticHint = false;

public:
    SceneNodeType sceneNodeType;

    QString name;
    /// Process-unique, monotonic, and 64 bits WHATEVER the platform: this was
    /// `long`, which is 32 bits on Windows LLP64 (deep audit 2026-09, area 5).
    /// SceneMirror keys its entry map on it, so a wrap is a silent aliasing of
    /// two different nodes onto one engine entry.
    qint64 nodeId;

    /// The scene this node belongs to, weakly (a scene owns its nodes through
    /// its registries; a node must never keep its scene alive).
    SceneWPtr scene;

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
    // its normal are all derived from the mesh's own bounds.
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
    virtual ~SceneNode();

    static SceneNodePtr create();

    // ---- the handle itself ------------------------------------------------
    /// This node's Ogre node. The mirror hands it to the engine (adoptNode) so
    /// that the engine's Item rides the DOCUMENT's node — which is what makes
    /// the per-frame transform push (audit F1) unnecessary.
    graph::NodeHandle graphNode() const { return mGraphNode; }
    /// Called by iris::graph when a migration rebuilds this node in another
    /// scene manager, and by its recursive destroy. Not for general use.
    void _setGraphNode(graph::NodeHandle h) { mGraphNode = h; ++mGraphEpoch; }
    /// Bumped every time the handle is REPLACED. The pointer alone cannot be
    /// used to notice that: Ogre recycles node memory, so a migrate out and
    /// back lands the rebuilt node on the very address the old one had (found
    /// by mirror.document_to_engine's reparent case — the cube went invisible
    /// because the mirror kept an engine Item attached to a destroyed node).
    quint32 graphEpoch() const { return mGraphEpoch; }
    /// Rebuilds this subtree inside `target` (an engine scene's manager, or the
    /// staging one). Scene::setGraphScene is the only caller.
    void _migrateGraph(graph::SceneHandle target, graph::NodeHandle newParent);

    // ---- the mutation funnel ----------------------------------------------
    using ChangeObserver = void (*)(SceneNode *, NodeChange);
    /// A single process-wide observer, null by default (v1 keeps the seam
    /// minimal deliberately — SPECS/SCENEGRAPH_SPEC.md §3 step 1). Every setter
    /// on this class routes through notifyChanged() before it forwards.
    ///
    /// The pointer is exposed so notifyChanged can be INLINE: this is on the
    /// path of every transform write in the program, and in a Debug build (the
    /// one the §6 benchmark measures) an out-of-line call whose body is one
    /// null test costs more than the test.
    static ChangeObserver sChangeObserver;
    static void setChangeObserver(ChangeObserver observer);
    static ChangeObserver changeObserver() { return sChangeObserver; }

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

    iris::Vec3 getLocalPos()   { return graph::localPos(mGraphNode); }
    iris::Quat getLocalRot()   { return graph::localRot(mGraphNode); }
    iris::Vec3 getLocalScale() { return graph::localScale(mGraphNode); }

	void rotate(iris::Quat rot, bool global = false);

    bool hasChildren() { return graph::childCount(mGraphNode) > 0; }

    void setLocalPos(iris::Vec3 pos);
    void setLocalRot(iris::Quat rot);
    void setLocalScale(iris::Vec3 scale);

    void setLocalTransform(iris::Mat4 transformMatrix);

    bool isAttached();
    void setAttached(bool attached);

	/// The owning parent. Null for a root node and for a detached one. Derived
	/// from the ONE tree — Ogre's — through the node's back-pointer.
	SceneNodePtr getParent() const;

	/// The scene this node belongs to, locked. Null when the node is detached,
	/// and null once the scene itself is gone.
	ScenePtr getScene() const
	{
		return scene.lock();
	}

	bool hasParent() const;
	bool hasScene()  const { return !scene.isNull(); }

    /// The children, in order, resolved through Ogre's child vector and the
    /// back-pointers. Built on demand: this is a QList by value, not a
    /// reference to a member, because there is no member any more.
    ///
    /// COSTS AN ALLOCATION AND N ATOMIC INCREMENTS. That is fine for a one-shot
    /// (a panel rebuilding a tree, a serializer, an exporter) and it is not
    /// fine inside a walk that runs every frame or over the whole document —
    /// use childCount()/childAt() there, which read Ogre's child vector and the
    /// owner back-pointers directly and allocate nothing. The mirror's per-sync
    /// walk was the biggest single cost in an idle frame until it stopped
    /// calling this.
    QList<SceneNodePtr> children() const;
    /// The child count without materialising the list.
    int childCount() const { return int(graph::childCount(mGraphNode)); }
    /// The i'th DOCUMENT child, borrowed — null when `i` names one of the
    /// engine's own children (a light's -Y adapter, a decal box, a wire), which
    /// share the tree and are not part of the document. Callers that walk
    /// SKIP nulls; they must not assume childAt(0..childCount()) is dense.
    ///
    /// Borrowed, not shared: a walk that only reads has no business bumping a
    /// refcount per node, and the caller's own reference to the root keeps the
    /// subtree alive. Do not store the result.
    SceneNode *childAt(int i) const
    {
        return graph::ownerOf(graph::childAt(mGraphNode, std::size_t(i)));
    }
    /// This node's position among its siblings, or -1. Undo captures it.
    int siblingIndex() const { return graph::indexInParent(mGraphNode); }

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

	void setVisible(bool flag = true);

    void show(bool cascade = false);
    void hide(bool cascade = false);

    bool isRemovable() {
        return removable;
    }

    void setPickable(bool canPick) {
        pickable = canPick;
        // Straight through to this node's engine objects as Ogre QUERY FLAGS:
        // picking's broad phase is a masked RaySceneQuery (SCENEGRAPH_SPEC §2)
        // and the mask is tested inside the sweep, so a node that stopped being
        // pickable has to say so before the next pick, not before the next
        // mirror sync. (The mirror pushes it too — that is the path for an Item
        // that did not exist yet when the flag was set.)
        graph::setPickable(mGraphNode, canPick);
        notifyChanged(NodeChange::Flags);
    }

    bool isPickable() {
        return pickable;
    }

    void setShadowCastingEnabled(bool val) {
        castShadow = val;
        notifyChanged(NodeChange::Flags);
    }

    bool getShadowCastingEnabled() {
        return castShadow;
    }

    void setPlanarReflector(bool val) {
        planarReflector = val;
        notifyChanged(NodeChange::Flags);
    }

    bool getPlanarReflector() const {
        return planarReflector;
    }

    /// SCENE_STATIC (SPECS/SCENEGRAPH_SPEC.md §6): "this node and everything
    /// under it never moves". Static nodes are skipped by Ogre's per-frame
    /// transform AND bounds passes entirely, which is where the multiplier of
    /// the whole swap lives (the passes run for them only on the frames after
    /// something changed — the engine batches that itself).
    ///
    /// The hint is a DOCUMENT FIELD, not a read of the graph, for two reasons:
    /// it survives a reparent (Ogre gives a node its parent's memory-manager
    /// class whenever the parent changes, so a graph-derived hint was silently
    /// lost by every insertChild — iris::graph re-applies the field instead),
    /// and it is what the serializer writes.
    ///
    /// THREE RULES, all in nodegraph.h and all enforced by iris::graph:
    ///  * it applies to the WHOLE SUBTREE. Ogre cannot do otherwise.
    ///  * a static node's parent must be static or be the scene's root node.
    ///    Setting the hint on a node under a moving parent is refused, loudly.
    ///  * MOVING A STATIC NODE UNDOES IT. The first transform write through
    ///    the funnel demotes the subtree back to dynamic and clears this flag,
    ///    which is what keeps a dragged node from paying a static pass a frame.
    ///
    /// Nodes whose engine attachment cannot change class — lights and particle
    /// systems (their object memory managers have no static twin) — refuse the
    /// hint. isStaticEligible() is the document-side test.
    void setStaticHint(bool value);
    bool staticHint() const { return mStaticHint; }
    /// What iris::graph reads when it reconciles a subtree after a structural
    /// move. Same value as staticHint(); named for the question it answers.
    bool wantsStatic() const { return mStaticHint; }
    /// Cleared by iris::graph when a transform write demotes this subtree.
    /// Not a public setter: it must not re-enter the graph.
    void _clearStaticHint() { mStaticHint = false; }
    /// Is this node the KIND of thing that may be static at all? Never-animated
    /// geometry and plain groupings only: a light, a particle system, a decal,
    /// a camera or a viewer carries an engine object that cannot switch class;
    /// a physics body, a socket rider, a skinned mesh and anything holding an
    /// animation are all going to move.
    virtual bool isStaticEligible() const;
    /// True when the graph really did put this node in the static manager —
    /// the hint is what was ASKED for, this is what happened.
    bool isStaticInGraph() const { return graph::isStatic(mGraphNode); }

    /// THE DEFAULT POLICY: mark this subtree static wherever it is safe to.
    ///
    /// Called when a subtree JOINS a scene — a primitive from the Add menu, an
    /// imported model, a project as it finishes loading — i.e. at the moments
    /// the document knows a branch is complete and at rest. Top-down, so a
    /// node is only marked once its parent already is (rule 2), and it marks
    /// nothing that isStaticEligible() refuses.
    ///
    /// Marking is not a decision the user has to make and cannot get wrong:
    /// the first transform write demotes whatever it touches (rule 4), so the
    /// worst case of an over-eager default is one subtree migration the first
    /// time something moves. The BEST case is the ground, the architecture and
    /// every imported prop dropping out of the engine's per-frame transform and
    /// bounds passes for the life of the session.
    ///
    /// NOT SERIALIZED YET. The hint is re-derived on load rather than written,
    /// which is why this is called from the reader; serializer v2 should
    /// persist an explicit user override on top of it.
    void applyStaticDefaults();

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
private:
    /// removeChild's body. `detachGraph` is false for the reparent path, which
    /// hands the node straight to its new parent — detaching it to the staging
    /// manager first would rebuild the whole subtree twice.
    void removeChildInternal(const SceneNodePtr &node, bool detachGraph);
public:

    bool isRootNode();

	iris::Quat getGlobalRotation() { return graph::globalRot(mGraphNode); }
    iris::Vec3 getGlobalPosition() { return graph::globalPos(mGraphNode); }
    iris::Mat4 getGlobalTransform() { return graph::globalTransform(mGraphNode); }
    iris::Mat4 getLocalTransform()  { return graph::localTransform(mGraphNode); }

	void setGlobalPos(iris::Vec3 pos);
	void setGlobalRot(iris::Quat rot);
	void setGlobalTransform(iris::Mat4 transform);

    /*
     * Per-node tick. NO transform work happens here any more — Ogre resolves
     * the hierarchy, in SIMD, on its worker threads, during the frame. What is
     * left is the subclasses' own per-frame business (cameras recompute their
     * matrices) and the walk that reaches them.
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
    /// "authored" is not the same as "current" once a different clip has run.
    ///
    /// Captured by applyDefaultPose(), whose call sites are exactly the moments
    /// a fragment is known to be at rest (scene load and fragment import).
    /// `hasRest` is false for a node nobody ever captured, and callers then
    /// fall back to the live transform.
    bool        hasRest = false;
    iris::Vec3   restPos;
    iris::Quat restRot;
    iris::Vec3   restScale{1, 1, 1};

protected:
    /// THE funnel. Every mutator calls it before (or instead of) forwarding.
    void notifyChanged(NodeChange what)
    {
        if (sChangeObserver) sChangeObserver(this, what);
    }

private:
    void setScene(ScenePtr scene);
    void removeFromScene();

    static qint64 generateNodeId();
};

}

Q_DECLARE_METATYPE(iris::SceneNodePtr)
#endif // SCENENODE_H
