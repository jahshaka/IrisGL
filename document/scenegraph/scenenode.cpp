/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include <QQuaternion>
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

#include <QUuid>

#include <atomic>

namespace iris
{

/// The node-id counter. It was a plain `static long nextId` incremented with
/// `nextId++` — not atomic, so two threads creating nodes could hand out the
/// same id, and 32-bit on Windows LLP64 besides (deep audit 2026-09, area 5).
/// Relaxed ordering is enough: the only requirement is that no two calls
/// return the same value, not that ids order anything.
static std::atomic<qint64> sNextNodeId{0};

SceneNode::SceneNode():
    pos(QVector3D(0,0,0)),
    scale(QVector3D(1,1,1)),
    rot(QQuaternion())

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

    localTransform.setToIdentity();
    globalTransform.setToIdentity();

    attached = false;

    transformDirty = true;
    globalDirty = false;      // transformDirty already forces the global
    hasDirtyChildren = true;

    //keyFrameSet = KeyFrameSet::create();
    //animation = iris::Animation::create("");
	setGUID(IrisUtils::generateGUID());
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
}

qint64 SceneNode::getNodeId()
{
    return nodeId;
}

void SceneNode::rotate(QQuaternion rot, bool global)
{
	if (global)
		this->rot = this->rot * rot;
	else
		this->rot = rot * this->rot;
	setTransformDirty();
}

void SceneNode::setLocalPos(QVector3D pos)
{
    this->pos = pos;
    setTransformDirty();
}

void SceneNode::setLocalRot(QQuaternion rot)
{
    this->rot = rot;
    setTransformDirty();
}

void SceneNode::setLocalScale(QVector3D scale)
{
    this->scale = scale;
    setTransformDirty();
}

void SceneNode::setLocalTransform(QMatrix4x4 transformMatrix)
{
    MathHelper::decomposeMatrix(transformMatrix, pos, rot, scale);
    setTransformDirty();
}

/// This node's own TRS changed. The flag propagates UPWARDS as
/// hasDirtyChildren so that update() descends to us from the root; the
/// downward half — every descendant's globalTransform is now stale too — is
/// update()'s job, because it is the only place that knows the new world
/// transform.
void SceneNode::setTransformDirty()
{
    transformDirty = true;
    if (auto p = getParent())
    {
        p->setHasDirtyChildren();
    }
}

void SceneNode::setHasDirtyChildren()
{
    hasDirtyChildren = true;
    if (auto p = getParent())
    {
        p->setHasDirtyChildren();
    }
}

bool SceneNode::isAttached()
{
    return attached;
}

void SceneNode::setAttached(bool attached)
{
    this->attached = attached;
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
    prop->value = pos;
    props.append(prop);

    prop = new Vec3Property();
    prop->displayName = "Rotation";
    prop->name = "rotation";
    prop->value = rot.toEulerAngles();
    props.append(prop);

    prop = new Vec3Property();
    prop->displayName = "Scale";
    prop->name = "scale";
    prop->value = scale;
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
    // (PLANAR_REFLECTIONS_SPEC.md §7): the flag is the only way a user gets a
    // mirror, and an author cannot be expected to hunt for it.
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
    if (valueName == "position") return pos;
    if (valueName == "rotation") return rot.toEulerAngles();
    if (valueName == "scale")	 return scale;
    if (valueName == "name")       return getName();
    if (valueName == "visible")    return isVisible();
    if (valueName == "castShadow") return getShadowCastingEnabled();
    if (valueName == "planarReflector") return getPlanarReflector();
    if (valueName == "pickable")   return isPickable();

    return QVariant();
}

bool SceneNode::setPropertyValue(QString valueName, const QVariant &value)
{
    if (valueName == "position") { setLocalPos(value.value<QVector3D>());   return true; }
    if (valueName == "rotation") { setLocalRot(QQuaternion::fromEulerAngles(value.value<QVector3D>())); return true; }
    if (valueName == "scale")    { setLocalScale(value.value<QVector3D>()); return true; }
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
    insertChild(children.size(), node, keepTransform);
}

void SceneNode::insertChild(int position, SceneNodePtr node, bool keepTransform)
{
    auto initialGlobalTransform = node->getGlobalTransform();

    if (node->hasParent()) {
        node->removeFromParent();
    }

    // @TODO: check if child is already a node
    auto self = sharedFromThis();

    children.insert(position, node);
    node->setParent(self);
    if (auto sc = getScene()) {
        node->setScene(sc);
        //scene->addNode(node);
    }

    if (keepTransform) {
        // @TODO: ensure global transform is calculated
        // this->update(0);///shortcut for now
        auto thisGlobalTransform = this->getGlobalTransform();

        //auto diff = initialGlobalTransform * thisGlobalTransform.inverted();
        auto diff = thisGlobalTransform.inverted() * initialGlobalTransform;

        auto pos = diff.column(3).toVector3D();
        node->pos = pos;
        node->rot = QQuaternion::fromRotationMatrix(diff.normalMatrix()).normalized();

        auto data = diff.data();

        // extracts the scale from the transform
        //node->scale = QVector3D(data[0], data[5], data[10]);
        node->scale.setX(diff.column(0).toVector3D().length());
        node->scale.setY(diff.column(1).toVector3D().length());
        node->scale.setZ(diff.column(2).toVector3D().length());
    }

    // Unconditionally: a reparent changes the node's world transform even when
    // its local one is untouched, and the keepTransform branch above writes
    // pos/rot/scale directly. This also marks the new parent chain, which is
    // how a freshly added subtree gets visited at all.
    node->setTransformDirty();
}

void SceneNode::removeFromParent()
{
    auto self = sharedFromThis();

    if (auto p = getParent()) p->removeChild(self);
}

void SceneNode::removeChild(SceneNodePtr node)
{
    children.removeOne(node);
    node->parent.clear();
    // Losing a parent changes the node's world transform (its local one is now
    // its global one) — it has to recompose.
    node->setTransformDirty();
    node->removeFromScene();
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
    // of different lengths. (The old `length > 60 → time × 1000` hack is gone:
    // Mesh::extractAnimations now converts assimp ticks to seconds at the
    // source, so scene time and key time share one unit.)
    const float sceneTime = time;

    if (!!animation) {
        time = animation->getSampleTime(time);
        // These write pos/rot/scale DIRECTLY rather than through the setters,
        // so they are the one mutator path that has to remember the flag
        // itself (it did not, and could not be noticed while the flags were
        // never cleared).
        bool posed = false;
        if (animation->hasPropertyAnim("position")) {
            pos = animation->getVector3PropertyAnim("position")->getValue(time);
            posed = true;
        }
        if (animation->hasPropertyAnim("rotation")) {
            auto r = animation->getVector3PropertyAnim("rotation")->getValue(time);
            rot = QQuaternion::fromEulerAngles(r);
            posed = true;
        }
        if (animation->hasPropertyAnim("scale")) {
            scale = animation->getVector3PropertyAnim("scale")->getValue(time);
            posed = true;
        }
        if (posed) setTransformDirty();
        // The SKELETAL branch is gone (ANIMATION_ENGINE_MIGRATION_SPEC, full
        // retirement). It used to walk the scene-node hierarchy by name,
        // overwrite every bone node's local transform from the clip, compose
        // skeleton-space matrices into a QMap and hand them to
        // Skeleton::applyAnimation to produce skin matrices — every frame, per
        // character, on one thread. Clip evaluation is the engine's now:
        // SceneMirror translates each clip ONCE and then states only which clip
        // is active and at what absolute time, and Ogre's threaded SIMD FK does
        // the rest. What the document keeps is the authored data and the clock.
    }

    for (const auto &child : children) {
        child->updateAnimation(sceneTime);
    }
}

void SceneNode::applyDefaultPose()
{
    // WHAT THIS IS NOW. The subtree is at rest RIGHT NOW — that is what every
    // one of this function's call sites means (scene load, fragment import) —
    // so it snapshots each node's authored local transform while that is still
    // true. Clip translation needs "the transform this node has when no clip is
    // driving it", and once a clip has played the live transform is not that
    // any more.
    //
    // WHAT IT USED TO BE. The same walk as updateAnimation's skeletal branch
    // with the key-write step removed: it composed the authored rest transforms
    // into skeleton-space matrices and pushed them through
    // Skeleton::applyAnimation to fill Skeleton::boneTransforms. Nothing reads
    // those any more — the engine resets an untracked bone to its BIND pose,
    // which is the same thing for every file that does not disagree with itself
    // (and SceneMirror logs the ones that do).
    hasRest = true;
    restPos = pos;
    restRot = rot;
    restScale = scale;

    for (const auto &child : children) {
        child->applyDefaultPose();
    }
}

void SceneNode::update(float dt)
{
    if (transformDirty) {
        localTransform.setToIdentity();

        localTransform.translate(pos);
        localTransform.rotate(rot);
        localTransform.scale(scale);
    }

    if (transformDirty || globalDirty) {
        if (auto p = getParent()) {
            globalTransform = p->globalTransform * localTransform;
        } else {
            globalTransform = localTransform;
        }

        // Our world transform moved, so every descendant's did. This is the
        // downward half of the invalidation, and the reason a moved node is
        // enough to refresh its whole subtree: setTransformDirty only ever
        // walked up.
        if (!children.isEmpty()) {
            for (const auto &child : children) child->globalDirty = true;
            hasDirtyChildren = true;
        }
    }

    if (hasDirtyChildren) {
        for (const auto &child : children) {
            child->update(dt);
        }
    }

    // Cleared AFTER the descent, never before: `hasDirtyChildren` is what the
    // loop above tests, and the flag this node may have just set for itself
    // (because its own world transform moved) has to survive until the loop
    // has used it.
    transformDirty = false;
    globalDirty = false;
    hasDirtyChildren = false;
}

void SceneNode::setParent(SceneNodePtr node)
{
    this->parent = node;
}

void SceneNode::setScene(ScenePtr scene)
{
    // should not already be a part of scene
    Q_ASSERT(!hasScene());

    this->scene = scene.toWeakRef();
    scene->addNode(this->sharedFromThis());

    // add children
    for (const auto &child : children) {
        child->setScene(scene);
    }
}

void SceneNode::removeFromScene()
{
    // The scene may already be gone — the link is weak now, so "my scene died
    // first" is a reachable state (it was not while the link kept the scene
    // alive). Nothing to unregister from in that case.
    auto sc = getScene();
    this->scene.clear();
    if (sc) sc->removeNode(this->sharedFromThis());

    // ...and the children
    for (const auto &child : children) {
        child->removeFromScene();
    }
}

qint64 SceneNode::generateNodeId()
{
    return sNextNodeId.fetch_add(1, std::memory_order_relaxed);
}

QQuaternion SceneNode::getGlobalRotation()
{
	if (auto p = getParent()) return p->getGlobalRotation() * rot;
	return rot;
}

QVector3D SceneNode::getGlobalPosition()
{
    return getGlobalTransform().column(3).toVector3D();
}

QMatrix4x4 SceneNode::getGlobalTransform()
{
    localTransform.setToIdentity();

    localTransform.translate(pos);
    localTransform.rotate(rot);
    localTransform.scale(scale);

    if (auto p = getParent()) {
        globalTransform = p->getGlobalTransform() * localTransform;
    } else {
        // this is a check for the root node
        globalTransform = localTransform;
    }

    return globalTransform;
}

QMatrix4x4 SceneNode::getLocalTransform()
{
    localTransform.setToIdentity();

    localTransform.translate(pos);
    localTransform.rotate(rot);
    localTransform.scale(scale);

    return localTransform;
}

void SceneNode::setGlobalPos(QVector3D pos)
{
	auto p = getParent();
	if (!p) {
		this->pos = pos;
		this->setTransformDirty();
		return;
	}

	auto globInv = p->getGlobalTransform().inverted();

	auto res = globInv * pos;

	this->pos = res;
	this->setTransformDirty();
}

void SceneNode::setGlobalRot(QQuaternion rot)
{
	auto p = getParent();
	if (!p) {
		this->rot = rot;
		this->setTransformDirty();
		return;
	}

	auto globInv = p->getGlobalRotation().inverted();
	auto res = globInv * rot;

	this->rot = res;
	this->setTransformDirty();
}

void SceneNode::setGlobalTransform(QMatrix4x4 transform)
{
	auto p = getParent();
	if (!p) {
		this->setLocalTransform(transform);
		return;
	}

	auto globInv = p->getGlobalTransform().inverted();
	auto res = globInv * transform;
	this->setLocalTransform(res);

	this->setTransformDirty();
}


SceneNodePtr SceneNode::duplicate()
{
    if (!duplicable) return SceneNodePtr();

    auto node = this->createDuplicate();

    node->setName(this->getName());
    node->setLocalPos(this->pos);
    node->setLocalScale(this->scale);
    node->setLocalRot(this->rot);
	node->castShadow	= this->castShadow;
	node->duplicable	= this->duplicable;
	node->visible		= this->visible;
	node->removable		= this->removable;
	node->pickable		= this->pickable;
	node->castShadow	= this->castShadow;
	node->planarReflector = this->planarReflector;
	node->attached		= this->attached;

    auto id = QUuid::createUuid();
    auto guid = id.toString().remove(0, 1);
    guid.chop(1);
    node->setGUID(guid);

    for (auto &child : this->children) {
        if (child->isDuplicable()) {
            node->addChild(child->duplicate(), false);
        }
    }

    return node.staticCast<SceneNode>();
}

}
