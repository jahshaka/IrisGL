/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "core/math/trs.h"
#include "core/math/qtinterop.h"
#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "document/scenegraph/scenenode.h"

#include <functional>

#include "document/animation/animation.h"
#include "document/animation/animableproperty.h"
#include "document/animation/keyframeanimation.h"
#include "document/animation/keyframeset.h"
#include "document/animation/propertyanim.h"
#include "document/animation/skeletalanimation.h"
#include "core/properties/property.h"
#include "document/assets/mesh.h"
#include "document/assets/skeleton.h"
#include "core/math/mathhelper.h"
#include "document/scenegraph/scene.h"
#include "document/scenegraph/meshnode.h"

#include <QDebug>
#include <QUuid>

#include <atomic>

namespace iris
{

/// The node-id counter. Relaxed ordering is enough: the only requirement is
/// that no two calls return the same value, not that ids order anything.
static std::atomic<qint64> sNextNodeId{0};

SceneNode::ChangeObserver SceneNode::sChangeObserver = nullptr;

void SceneNode::setChangeObserver(ChangeObserver observer) { sChangeObserver = observer; }

SceneNode::SceneNode()
{
    sceneNodeType = SceneNodeType::Empty;
    nodeId = generateNodeId();
    setName(QString("SceneNode%1").arg(nodeId));

    visible = true;
    duplicable = true;
    removable = true;
	exportable = true;
    isBuiltIn = false;
	isPhysicsBody = false;

    pickable = true;
	pickingGroups = 0;
    castShadow = true;

    attached = false;

    // THE handle. Every node is born detached, in the staging scene manager
    // (SPECS/SCENEGRAPH_SPEC.md §4: "fragments build as detached handle trees,
    // attach on commit"); addChild moves it into whatever manager its new
    // parent lives in.
    mGraphNode = graph::createNode(graph::stagingScene(), nullptr, this);
    if (!mGraphNode) {
        // v1 consequence, stated honestly rather than papered over: the
        // document graph IS Ogre's, so an Ogre::Root must exist before the
        // first node. Headless paths boot the engine offscreen until v2's NULL
        // render system lands (spec §3).
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning("iris::SceneNode: no scene graph device — an Ogre::Root must exist "
                     "before the first document node is created (SCENEGRAPH_SPEC v1). "
                     "Every transform on this node will read as identity.");
        }
    }

	setGUID(IrisUtils::generateGUID());
}

SceneNode::~SceneNode()
{
    // Deepest-first, through the one sanctioned path. The children's handles
    // are cleared on the way, so the QSharedPointers in mChildRefs (released
    // after this body runs) find nothing left to destroy.
    if (mGraphNode) {
        graph::destroyNode(mGraphNode);
        mGraphNode = nullptr;
    }
}

SceneNodePtr SceneNode::create()
{
    return QSharedPointer<SceneNode>(new SceneNode());
}

QString SceneNode::getName()
{
    return name;
}

void SceneNode::setName(QString name)
{
    this->name = name;
    notifyChanged(NodeChange::Name);
}

qint64 SceneNode::getNodeId()
{
    return nodeId;
}

// ---- structure -------------------------------------------------------------

SceneNodePtr SceneNode::getParent() const
{
    SceneNode *p = graph::ownerOf(graph::parentOf(mGraphNode));
    return p ? p->sharedFromThis() : SceneNodePtr();
}

bool SceneNode::hasParent() const
{
    return graph::ownerOf(graph::parentOf(mGraphNode)) != nullptr;
}

QList<SceneNodePtr> SceneNode::children() const
{
    QList<SceneNodePtr> out;
    const std::size_t n = graph::childCount(mGraphNode);
    out.reserve(int(n));
    for (std::size_t i = 0; i < n; ++i) {
        // Engine-owned children (a light's -Y adapter, a decal's projector box,
        // a range wire) share the tree and have no document owner: they are not
        // part of the document and never appear here.
        if (SceneNode *o = graph::ownerOf(graph::childAt(mGraphNode, i)))
            out.append(o->sharedFromThis());
    }
    return out;
}

void SceneNode::_migrateGraph(graph::SceneHandle target, graph::NodeHandle newParent)
{
    if (!mGraphNode) return;
    mGraphNode = graph::migrate(mGraphNode, target, newParent);
}

void SceneNode::rotate(iris::Quat rot, bool global)
{
    const iris::Quat cur = graph::localRot(mGraphNode);
    notifyChanged(NodeChange::Transform);
    graph::setLocalRot(mGraphNode, global ? cur * rot : rot * cur);
}

void SceneNode::setLocalPos(iris::Vec3 pos)
{
    notifyChanged(NodeChange::Transform);
    graph::setLocalPos(mGraphNode, pos);
}

void SceneNode::setLocalRot(iris::Quat rot)
{
    notifyChanged(NodeChange::Transform);
    graph::setLocalRot(mGraphNode, rot);
}

void SceneNode::setLocalScale(iris::Vec3 scale)
{
    notifyChanged(NodeChange::Transform);
    graph::setLocalScale(mGraphNode, scale);
}

void SceneNode::setLocalTransform(iris::Mat4 transformMatrix)
{
    iris::Vec3 p, s;
    iris::Quat r;
    MathHelper::decomposeMatrix(transformMatrix, p, r, s);
    notifyChanged(NodeChange::Transform);
    graph::setLocalTrs(mGraphNode, p, r, s);
}

bool SceneNode::isAttached()
{
    return attached;
}

void SceneNode::setAttached(bool attached)
{
    this->attached = attached;
    notifyChanged(NodeChange::Flags);
}

void SceneNode::setVisible(bool flag)
{
    visible = flag;
    notifyChanged(NodeChange::Visibility);
}

void SceneNode::show(bool cascade)
{
    setVisible(true);
    if (cascade) {
        const int n = childCount();
        for (int i = 0; i < n; ++i) if (SceneNode *c = childAt(i)) c->show(cascade);
    }
}

void SceneNode::hide(bool cascade)
{
    setVisible(false);
    if (cascade) {
        const int n = childCount();
        for (int i = 0; i < n; ++i) if (SceneNode *c = childAt(i)) c->hide(cascade);
    }
}

bool SceneNode::isStaticEligible() const
{
    // Node kinds whose engine attachment cannot change memory-manager class.
    switch (sceneNodeType) {
    case SceneNodeType::Light:
    case SceneNodeType::ParticleSystem:
    case SceneNodeType::Decal:
    case SceneNodeType::Camera:
    case SceneNodeType::Viewer:
        return false;
    case SceneNodeType::Empty:
    case SceneNodeType::Mesh:
        break;
    }
    if (isPhysicsBody) return false;         // Bullet writes its transform every step
    if (isSocketAttached()) return false;    // the socket resolver writes it every frame
    if (!animations.isEmpty() || !animation.isNull()) return false;
    return true;
}

void SceneNode::setStaticHint(bool value)
{
    // The user's word, recorded BEFORE the eligibility test: a refusal is still
    // an opinion the file should carry ("I want this static") and it becomes
    // legal the moment the node stops being a physics body, loses its clip or
    // moves under a static parent. The refusal below leaves the graph alone; it
    // does not un-record the intent.
    mStaticOverride = value ? StaticOverride::Static : StaticOverride::Dynamic;
    _applyStaticHint(value);
}

void SceneNode::_applyStaticHint(bool value)
{
    if (value && !isStaticEligible()) {
        qWarning("iris::SceneNode::setStaticHint(true) refused for '%s': this node kind moves "
                 "(SCENEGRAPH_SPEC §6 — lights, particles, decals, cameras, viewers, physics "
                 "bodies, socket riders and animated nodes are never static).",
                 qPrintable(name));
        return;
    }
    // The GRAPH state is part of the test, not just the field: a node that
    // inherited static from a static parent has `mStaticHint == false` while
    // sitting in the static manager, and setStaticHint(false) on it must
    // really demote it.
    if (mStaticHint == value && graph::isStatic(mGraphNode) == value) return;
    mStaticHint = value;
    // The graph may refuse (an ineligible parent, an attachment that cannot
    // switch): the document then remembers what was asked for, because a later
    // reparent under a static parent makes the same request legal. `staticHint`
    // is the intent; `isStaticInGraph()` is the outcome.
    graph::setStatic(mGraphNode, value);
    notifyChanged(NodeChange::Flags);
}

void SceneNode::applyStaticDefaults()
{
    // A HUMAN'S DECISION BEATS THE POLICY (StaticOverride). `Dynamic` leaves
    // this node moving — and, through canBeStatic, its whole branch with it,
    // which is exactly rule 2 doing the right thing for free. `Static` is
    // re-asserted rather than skipped: the node may have arrived here through a
    // reparent that reset its memory-manager class.
    switch (mStaticOverride) {
    case StaticOverride::Dynamic:
        _applyStaticHint(false);
        break;
    case StaticOverride::Static:
        _applyStaticHint(true);
        break;
    case StaticOverride::None:
        // canBeStatic() first: an ineligible PARENT means this whole branch
        // stays dynamic, and asking anyway would log a refusal per node.
        // applyStaticHint, not setStaticHint: the POLICY must never leave a
        // recorded "the user asked for this" behind (that would put the
        // derivation in the file on the next save — see StaticOverride).
        if (isStaticEligible() && graph::canBeStatic(mGraphNode)) _applyStaticHint(true);
        break;
    }
    // Descend regardless — a light in the middle of an imported rig does not
    // stop the props below it from being static, it only stops ITSELF (and,
    // through canBeStatic, the branch under it, which is the rule).
    const std::size_t n = graph::childCount(mGraphNode);
    for (std::size_t i = 0; i < n; ++i)
        if (SceneNode *c = graph::ownerOf(graph::childAt(mGraphNode, i)))
            c->applyStaticDefaults();
}

void SceneNode::addAnimation(AnimationPtr anim)
{
    animations.append(anim);
}

QList<AnimationPtr> SceneNode::getAnimations()
{
    return animations;
}

void SceneNode::setAnimation(AnimationPtr anim)
{
    animation = anim;
}

AnimationPtr SceneNode::getAnimation()
{
    return animation;
}

bool SceneNode::hasActiveAnimation()
{
    return !!animation;
}

void SceneNode::deleteAnimation(int index)
{
    animations.removeAt(index);
}

void SceneNode::deleteAnimation(AnimationPtr anim)
{
    animations.removeOne(anim);
}

QList<Property*> SceneNode::getProperties()
{
    auto props = QList<Property*>();

    auto prop = new Vec3Property();
    prop->displayName = "Position";
    prop->name = "position";
    prop->value = iris::toQt(getLocalPos());
    props.append(prop);

    prop = new Vec3Property();
    prop->displayName = "Rotation";
    prop->name = "rotation";
    prop->value = iris::toQt(getLocalRot().toEulerAngles());
    props.append(prop);

    prop = new Vec3Property();
    prop->displayName = "Scale";
    prop->name = "scale";
    prop->value = iris::toQt(getLocalScale());
    props.append(prop);

    // There is no StringProperty in core/properties/property.h; FileProperty is
    // the QString-valued Property, so the name rides on it.
    auto nameProp = new FileProperty();
    nameProp->displayName = "Name";
    nameProp->name = "name";
    nameProp->value = name;
    props.append(nameProp);

    auto boolProp = new BoolProperty();
    boolProp->displayName = "Visible";
    boolProp->name = "visible";
    boolProp->value = visible;
    props.append(boolProp);

    boolProp = new BoolProperty();
    boolProp->displayName = "Cast Shadow";
    boolProp->name = "castShadow";
    boolProp->value = castShadow;
    props.append(boolProp);

    // A TOP-LEVEL row beside Cast Shadow, not a buried Reflections section
    // (PLANAR_REFLECTIONS_SPEC.md §7).
    boolProp = new BoolProperty();
    boolProp->displayName = "Planar Reflector";
    boolProp->name = "planarReflector";
    boolProp->value = planarReflector;
    props.append(boolProp);

    boolProp = new BoolProperty();
    boolProp->displayName = "Pickable";
    boolProp->name = "pickable";
    boolProp->value = pickable;
    props.append(boolProp);

    return props;
}

QVariant SceneNode::getPropertyValue(QString valueName)
{
    if (valueName == "position") return iris::toQt(getLocalPos());
    if (valueName == "rotation") return iris::toQt(getLocalRot().toEulerAngles());
    if (valueName == "scale")	 return iris::toQt(getLocalScale());
    if (valueName == "name")       return getName();
    if (valueName == "visible")    return isVisible();
    if (valueName == "castShadow") return getShadowCastingEnabled();
    if (valueName == "planarReflector") return getPlanarReflector();
    if (valueName == "pickable")   return isPickable();

    return QVariant();
}

bool SceneNode::setPropertyValue(QString valueName, const QVariant &value)
{
    if (valueName == "position") { setLocalPos(iris::fromQt(value.value<QVector3D>()));   return true; }
    if (valueName == "rotation") { setLocalRot(iris::Quat::fromEulerAngles(iris::fromQt(value.value<QVector3D>()))); return true; }
    if (valueName == "scale")    { setLocalScale(iris::fromQt(value.value<QVector3D>())); return true; }
    if (valueName == "name")       { setName(value.toString());                  return true; }
    // setVisible, not show()/hide(): those cascade to children, which is a
    // different operation from setting this node's own flag.
    if (valueName == "visible")    { setVisible(value.toBool());                 return true; }
    if (valueName == "castShadow") { setShadowCastingEnabled(value.toBool());    return true; }
    if (valueName == "planarReflector") { setPlanarReflector(value.toBool());     return true; }
    if (valueName == "pickable")   { setPickable(value.toBool());                return true; }
    return false;
}

SceneNodeType SceneNode::getSceneNodeType()
{
    return sceneNodeType;
}

void SceneNode::addChild(SceneNodePtr node, bool keepTransform)
{
    // -1 = APPEND. Passing childCount() would be the same position but would
    // send insertChild's sibling-index path down a scan it does not need, on
    // every child of every node of every document build.
    insertChild(-1, node, keepTransform);
}

void SceneNode::insertChild(int position, SceneNodePtr node, bool keepTransform)
{
    if (!node) return;
    const iris::Mat4 initialGlobalTransform = node->getGlobalTransform();

    if (auto oldParent = node->getParent())
        oldParent->removeChildInternal(node, false);

    // ONE tree: the child moves inside Ogre's hierarchy. A child that lives in
    // a different scene manager (the staging one, which is where every node is
    // born) is REBUILT under us — an Ogre::SceneNode belongs to its creator and
    // cannot be handed to another manager.
    if (graph::sceneOf(node->mGraphNode) != graph::sceneOf(mGraphNode))
        node->_migrateGraph(graph::sceneOf(mGraphNode), mGraphNode);
    graph::attach(mGraphNode, node->mGraphNode, position);

    mChildRefs.append(node);      // the lifetime anchor; never the structure

    if (auto sc = getScene()) node->setScene(sc);

    if (keepTransform) {
        // ONE decomposition, Ogre's: the world transform the node had before
        // the move is re-expressed in the new parent's space. (The old code
        // took the rotation from diff.normalMatrix() — the inverse-transpose,
        // R * S^-1, which is R only at scale 1 — and the scale from the column
        // lengths, so reparenting a non-uniformly scaled node rotated it.)
        node->setGlobalTransform(initialGlobalTransform);
    }

    node->notifyChanged(NodeChange::Structure);
    notifyChanged(NodeChange::Structure);
}

void SceneNode::removeFromParent()
{
    auto self = sharedFromThis();
    if (auto p = getParent()) p->removeChild(self);
}

void SceneNode::removeChild(SceneNodePtr node)
{
    removeChildInternal(node, true);
}

void SceneNode::removeChildInternal(const SceneNodePtr &node, bool detachGraph)
{
    if (!node) return;
    // Out of the tree first (this is what makes it stop rendering and stop
    // being reachable), THEN out of the scene registries, THEN drop our
    // ownership — the caller's own SceneNodePtr is what keeps it alive.
    if (detachGraph) {
        // The scene has to be told BEFORE removeFromScene clears the link: the
        // subtree stays in that scene's scene manager and has to travel with it
        // when it unbinds (see Scene::rememberDetached).
        if (auto sc = node->getScene()) sc->rememberDetached(node);
        node->mGraphNode = graph::detach(node->mGraphNode);
    }
    node->removeFromScene();
    mChildRefs.removeOne(node);
    node->notifyChanged(NodeChange::Structure);
    notifyChanged(NodeChange::Structure);
}

bool SceneNode::isRootNode()
{
    auto sc = getScene();
    return sc && sc->getRootNode().data() == this;
}

void SceneNode::updateAnimation(float time)
{
    // Children sample the ORIGINAL scene time (SKELETAL_PLAYBACK_SPEC S5):
    // this node's loop-remapped time must not leak into sibling/nested clips
    // of different lengths.
    const float sceneTime = time;

    if (!!animation) {
        time = animation->getSampleTime(time);
        // Through the funnel, like every other mutation (SCENEGRAPH_SPEC §3
        // step 4): these used to write pos/rot/scale directly and had to
        // remember the dirty flag themselves.
        if (animation->hasPropertyAnim("position"))
            setLocalPos(animation->getVector3PropertyAnim("position")->getValue(time));
        if (animation->hasPropertyAnim("rotation"))
            setLocalRot(iris::Quat::fromEulerAngles(
                animation->getVector3PropertyAnim("rotation")->getValue(time)));
        if (animation->hasPropertyAnim("scale"))
            setLocalScale(animation->getVector3PropertyAnim("scale")->getValue(time));
        // The SKELETAL branch is gone (ANIMATION_ENGINE_MIGRATION_SPEC, full
        // retirement): clip evaluation is the engine's, and what the document
        // keeps is the authored data and the clock.
    }

    // childAt(), not children(): this runs for every node of the document on
    // every play-mode tick, and children() allocates a QList and bumps a
    // refcount per child to hand back what a raw walk reads for free.
    const int n = childCount();
    for (int i = 0; i < n; ++i)
        if (SceneNode *c = childAt(i)) c->updateAnimation(sceneTime);
}

void SceneNode::applyDefaultPose()
{
    // The subtree is at rest RIGHT NOW — that is what every one of this
    // function's call sites means (scene load, fragment import) — so it
    // snapshots each node's authored local transform while that is still true.
    hasRest = true;
    restPos = getLocalPos();
    restRot = getLocalRot();
    restScale = getLocalScale();

    const int n = childCount();
    for (int i = 0; i < n; ++i)
        if (SceneNode *c = childAt(i)) c->applyDefaultPose();
}

void SceneNode::update(float dt)
{
    // NO transform work. Composition, invalidation and propagation are Ogre's
    // (SIMD, threaded, inside the frame); what is left is the walk that lets
    // subclasses do their own per-frame business.
    // Per-frame, whole document: no allocation, no refcount (see childAt).
    const int n = childCount();
    for (int i = 0; i < n; ++i)
        if (SceneNode *c = childAt(i)) c->update(dt);
}

void SceneNode::setScene(ScenePtr scene)
{
    // should not already be a part of scene
    Q_ASSERT(!hasScene());

    this->scene = scene.toWeakRef();
    scene->addNode(this->sharedFromThis());

    // add children
    for (const auto &child : children()) {
        child->setScene(scene);
    }
}

void SceneNode::removeFromScene()
{
    auto sc = getScene();
    this->scene.clear();
    if (sc) sc->removeNode(this->sharedFromThis());

    // ...and the children
    for (const auto &child : children()) {
        child->removeFromScene();
    }
}

qint64 SceneNode::generateNodeId()
{
    return sNextNodeId.fetch_add(1, std::memory_order_relaxed);
}

void SceneNode::setGlobalPos(iris::Vec3 pos)
{
    notifyChanged(NodeChange::Transform);
    graph::setGlobalPos(mGraphNode, pos);
}

void SceneNode::setGlobalRot(iris::Quat rot)
{
    notifyChanged(NodeChange::Transform);
    graph::setGlobalRot(mGraphNode, rot);
}

void SceneNode::setGlobalTransform(iris::Mat4 transform)
{
    notifyChanged(NodeChange::Transform);
    graph::setGlobalTransform(mGraphNode, transform);
}

SceneNodePtr SceneNode::duplicate()
{
    QHash<QString, QString> guidMap;
    auto node = duplicateInto(guidMap);
    if (!node) return node;
    node->remapSocketOwners(guidMap);
    return node;
}

void SceneNode::remapSocketOwners(const QHash<QString, QString> &guidMap)
{
    if (!socketOwnerGuid.isEmpty()) {
        const auto it = guidMap.constFind(socketOwnerGuid);
        if (it != guidMap.constEnd()) socketOwnerGuid = it.value();
    }
    const int n = childCount();
    for (int i = 0; i < n; ++i) if (SceneNode *c = childAt(i)) c->remapSocketOwners(guidMap);
}

SceneNodePtr SceneNode::duplicateInto(QHash<QString, QString> &guidMap)
{
    if (!duplicable) return SceneNodePtr();

    auto node = this->createDuplicate();

    node->setName(this->getName());
    node->setLocalPos(this->getLocalPos());
    node->setLocalScale(this->getLocalScale());
    node->setLocalRot(this->getLocalRot());
	node->castShadow	= this->castShadow;
	node->duplicable	= this->duplicable;
	node->visible		= this->visible;
	node->removable		= this->removable;
	node->pickable		= this->pickable;
	node->planarReflector = this->planarReflector;
	node->attached		= this->attached;
    // The user's SCENE_STATIC decision travels with the copy (the derived hint
    // does not: the copy is about to be parented somewhere, and the policy pass
    // that follows every add re-derives it). Without this a duplicate of a node
    // the user had pinned Dynamic came back Static on the next load.
    node->_setStaticOverride(this->mStaticOverride);

    auto id = QUuid::createUuid();
    auto guid = id.toString().remove(0, 1);
    guid.chop(1);
    node->setGUID(guid);
    guidMap.insert(this->getGUID(), guid);

    // The attachment travels with the copy. Whether it points at the ORIGINAL
    // owner or at the copy's own is decided by remapSocketOwners once the whole
    // subtree is known — see the note on duplicateInto.
    node->setSocketAttachment(this->socketOwnerGuid, this->socketName);

    for (const auto &child : this->children()) {
        if (child->isDuplicable()) {
            node->addChild(child->duplicateInto(guidMap), false);
        }
    }

    return node.staticCast<SceneNode>();
}

}
