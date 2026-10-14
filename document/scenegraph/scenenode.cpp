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
#include "document/animation/locomotion.h"
#include "core/properties/property.h"
#include "document/assets/mesh.h"
#include "document/assets/skeleton.h"
#include "document/physics/avatarmovement.h"
#include "core/math/mathhelper.h"
#include "document/scenegraph/scene.h"
#include "document/scenegraph/meshnode.h"

#include <QDebug>
#include <QUuid>


namespace iris
{

/// The node-id counter. Relaxed ordering is enough: the only requirement is
/// that no two calls return the same value, not that ids order anything.
static std::atomic<qint64> sNextNodeId{0};

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

    // THE OWNER TABLE the write journal resolves this node's id through (nodegraph.h).
    graph::registerNode(this);

	setGUID(IrisUtils::generateGUID());
}

void SceneNode::noteDocumentWrite()
{
    graph::noteDocumentWrite(this);
}

void SceneNode::notifyChangedSubtree(NodeChange what)
{
    if (!mDirtySet) return;
    // PRUNED ON THE SUBTREE BIT, never on the plain one: this node has already
    // been cascaded for this kind, so its whole subtree carries the mark and a
    // second cascade over the same branch costs one test. (A reader that hides
    // a hundred nodes one by one inside one subtree would otherwise walk that
    // subtree a hundred times.)
    const quint16 sub = nodeSubtreeBit(what);
    if (mDirtyMask & sub) return;
    notifyChanged(what);
    mDirtyMask |= sub;
    // childAt(), not children(): a cascade over an 8,000-node subtree must not
    // allocate a QList and bump a refcount per child on the way.
    const int n = childCount();
    for (int i = 0; i < n; ++i)
        if (SceneNode *c = childAt(i)) c->notifyChangedSubtree(what);
}

SceneNode::~SceneNode()
{
    // The store first: out of its parent's list (a parent being destroyed is
    // releasing it anyway) and its children orphaned (they die with their refs).
    graph::unlinkDoc(mDoc);
    if (mDoc.pickTable) mDoc.pickTable->remove(this);
    graph::unregisterNode(this);
}

SceneNodePtr SceneNode::create()
{
    return QSharedPointer<SceneNode>(new SceneNode());
}

void SceneNode::setAvatarComponent(const AvatarMovementPtr &component)
{
    avatarMovement = component;
    notifyChanged(NodeChange::Flags);
}

void SceneNode::setLocomotionComponent(const AvatarLocomotionPtr &component)
{
    avatarLocomotion = component;
    notifyChanged(NodeChange::Flags);
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

qint64 SceneNode::getNodeId() const
{
    return nodeId;
}

// ---- structure -------------------------------------------------------------

SceneNodePtr SceneNode::getParent() const
{
    return mDoc.parent ? mDoc.parent->owner->sharedFromThis() : SceneNodePtr();
}

bool SceneNode::hasParent() const
{
    return mDoc.parent != nullptr;
}

QList<SceneNodePtr> SceneNode::children() const
{
    QList<SceneNodePtr> out;
    out.reserve(int(mDoc.children.size()));
    for (const graph::DocNode *c : mDoc.children) out.append(c->owner->sharedFromThis());
    return out;
}

void SceneNode::rotate(iris::Quat rot, bool global)
{
    const iris::Quat cur = mDoc.rot;
    notifyChanged(NodeChange::Transform);
    graph::writeLocalRot(mDoc, global ? cur * rot : rot * cur);
}

void SceneNode::setLocalPos(iris::Vec3 pos)
{
    notifyChanged(NodeChange::Transform);
    graph::writeLocalPos(mDoc, pos);
}

void SceneNode::setLocalRot(iris::Quat rot)
{
    notifyChanged(NodeChange::Transform);
    graph::writeLocalRot(mDoc, rot);
}

void SceneNode::setLocalScale(iris::Vec3 scale)
{
    notifyChanged(NodeChange::Transform);
    graph::writeLocalScale(mDoc, scale);
}

void SceneNode::setLocalTransform(iris::Mat4 transformMatrix)
{
    iris::Vec3 p, s;
    iris::Quat r;
    MathHelper::decomposeMatrix(transformMatrix, p, r, s);
    notifyChanged(NodeChange::Transform);
    graph::writeLocalTrs(mDoc, p, r, s);
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
    if (visible == flag) return;
    visible = flag;
    // EFFECTIVE VISIBILITY IS PARENT-AND and the mirror is its SOLE pusher
    // (ENGINE-3): hiding a model root has to mark every descendant, or they
    // stay drawn — and voxelised — with nothing to correct them.
    notifyChangedSubtree(NodeChange::Visibility);
}

bool SceneNode::isVisibleInScene() const
{
    // Raw pointers up the store's chain — no QSharedPointer per level. A socket
    // rider's parent is its document parent (socketing is not a reparent).
    for (const SceneNode *n = this; n; n = (n->mDoc.parent ? n->mDoc.parent->owner : nullptr))
        if (!n->visible) return false;
    return true;
}

// ---------------------------------------------------------------------------
// MOBILITY (SPECS/REALTIME_REFLECTIONS_SPEC.md §3.3) and the graph's static
// class, which is a different question — see scenenode.h's two doc blocks.
// ---------------------------------------------------------------------------

const char *mobilityName(Mobility m)
{
    switch (m) {
    case Mobility::Static:  return "static";
    case Mobility::Movable: return "movable";
    case Mobility::Auto:    break;
    }
    return "auto";
}

bool mobilityFromName(const QString &name, Mobility &out)
{
    const QString wanted = name.trimmed().toLower();
    if (wanted == QLatin1String("auto"))    { out = Mobility::Auto;    return true; }
    if (wanted == QLatin1String("static"))  { out = Mobility::Static;  return true; }
    if (wanted == QLatin1String("movable")) { out = Mobility::Movable; return true; }
    return false;
}

const char *mobilityReasonName(MobilityReason r)
{
    switch (r) {
    case MobilityReason::User:      return "user";
    case MobilityReason::Physics:   return "physics";
    case MobilityReason::Avatar:    return "avatar";
    case MobilityReason::Socket:    return "socket";
    case MobilityReason::Animation: return "animation";
    case MobilityReason::Skeleton:  return "skeleton";
    case MobilityReason::Particles: return "particles";
    case MobilityReason::Parent:    return "parent";
    case MobilityReason::Play:      return "play";
    case MobilityReason::Default:   break;
    }
    return "default";
}

namespace {
/// ANIMATED means "something writes this node's transform", not "an Animation
/// object is attached" — see isStaticEligible's note for the measurement that
/// made the distinction load-bearing. Split in two here because mobility
/// reports WHY: real property channels are `animation`, a skeletal clip is
/// `skeleton`.
bool drivesTransform(const AnimationPtr &a) { return !a.isNull() && !a->properties.isEmpty(); }
/// A SKELETAL CLIP DRIVES THE NODES IT NAMES, not the node that carries it
/// (BISTRO-IMPORT-1). An imported file's clips ride its fragment ROOT and their
/// channels are matched to scene nodes by name; the root itself moves only if a
/// channel names it and its keys move. (Before, carrying ANY clip made the carrier
/// movable — and rule 2 then made the Bistro's 5,942 meshes movable for a Take that
/// moved one camera.)
bool clipKeysNode(const AnimationPtr &a, const QString &name)
{
    return !a.isNull() && !a->skeletalAnimation.isNull() && a->skeletalAnimation->movesNode(name);
}
bool clipMovesAnything(const AnimationPtr &a)
{
    return !a.isNull() && !a->skeletalAnimation.isNull() && a->skeletalAnimation->movesAnything();
}
} // namespace

bool SceneNode::isKeyedByClip() const
{
    for (const SceneNode *n = this; n; n = (n->mDoc.parent ? n->mDoc.parent->owner : nullptr)) {
        if (clipKeysNode(n->animation, name)) return true;
        for (const AnimationPtr &a : n->animations)
            if (clipKeysNode(a, name)) return true;
    }
    return false;
}

bool SceneNode::hasMovingClipOver() const
{
    for (const SceneNode *n = this; n; n = (n->mDoc.parent ? n->mDoc.parent->owner : nullptr)) {
        if (clipMovesAnything(n->animation)) return true;
        for (const AnimationPtr &a : n->animations)
            if (clipMovesAnything(a)) return true;
    }
    return false;
}

bool SceneNode::hasMobilityDriver(MobilityReason *why) const
{
    const auto yes = [why](MobilityReason r) { if (why) *why = r; return true; };
    // A SIMULATED body: Bullet writes its transform every step.
    //
    // ALL THREE TESTS ARE LOAD-BEARING, and `isPhysicsBody` alone is none of
    // them:
    //
    //  * TYPE None. The Properties panel's Collision Shape row sets
    //    `isPhysicsBody = true` on its own (physicspropertywidget.cpp), leaving
    //    Physics Type at None and the mass at its constructor default of 1 — so
    //    any node whose shape was ever touched would read as moving, with the
    //    Movement blade saying "Moves - it is a physics object" directly above a
    //    Physics blade reading "None". A scene written before the file carried
    //    `physicsProperties.type` reads back None the same way
    //    (scenereader.cpp: a missing key is 0).
    //  * TYPE Static, and a ZERO MASS, which is the same thing in Bullet: an
    //    immovable body does not move. The DEFAULT SCENE'S GROUND is one — the
    //    thing a character walks on — so this decides whether the floor of every
    //    new project is classified as moving, and in lane R2 that is the floor
    //    leaving the reflection probes and the bounce light.
    //
    // Nothing writes an immovable body's transform but the author, and that is
    // an editor drag, which is not a promotion (§3.3.3).
    if (isPhysicsBody && physicsProperty.type != PhysicsType::None
        && physicsProperty.type != PhysicsType::Static
        && physicsProperty.objectMass != 0.0f)
        return yes(MobilityReason::Physics);
    // An avatar wrapper walks: the movement component and the locomotion state
    // machine both write it. (This was the gap in the old isStaticEligible —
    // an avatar was only caught later, by rule 4, after it had already moved.)
    if (hasAvatarComponent()) return yes(MobilityReason::Avatar);
    // The socket resolver writes a rider's transform every frame.
    if (isSocketAttached()) return yes(MobilityReason::Socket);
    if (sceneNodeType == SceneNodeType::ParticleSystem) return yes(MobilityReason::Particles);
    if (drivesTransform(animation)) return yes(MobilityReason::Animation);
    for (const AnimationPtr &a : animations)
        if (drivesTransform(a)) return yes(MobilityReason::Animation);
    // A clip over this node that KEYS it (its own or an ancestor's): an animation
    // writes this node's transform.
    if (isKeyedByClip()) return yes(MobilityReason::Animation);
    return false;
}

Mobility SceneNode::resolveMobility(bool parentMovable, MobilityReason *why) const
{
    MobilityReason driver = MobilityReason::Default;
    if (hasMobilityDriver(&driver)) { if (why) *why = driver; return Mobility::Movable; }
    if (parentMovable) { if (why) *why = MobilityReason::Parent; return Mobility::Movable; }
    if (mMobility != Mobility::Auto) {
        if (why) *why = MobilityReason::User;
        return mMobility;
    }
    // SOFT PROMOTION is the LAST word before the default and applies to `auto`
    // nodes only: a user who wrote "static" said something the engine keeps
    // honouring, and their ghost bounce light is their own decision.
    if (mSoftMovable) { if (why) *why = MobilityReason::Play; return Mobility::Movable; }
    if (why) *why = MobilityReason::Default;
    return Mobility::Static;
}

Mobility SceneNode::resolvedMobility(MobilityReason *why) const
{
    // Rule 2 walks UP: a node travels with its parent. parentOf answers a
    // socket rider with its document parent, which is right — a rider is
    // movable through rule 1 anyway.
    bool parentMovable = false;
    if (const SceneNode *p = mDoc.parent ? mDoc.parent->owner : nullptr)
        parentMovable = p->resolvedMobility(nullptr) == Mobility::Movable;
    return resolveMobility(parentMovable, why);
}

bool SceneNode::isStaticEligible() const
{
    // Node kinds whose engine attachment cannot change memory-manager class.
    switch (sceneNodeType) {
    case SceneNodeType::Light:
    case SceneNodeType::ParticleSystem:
    case SceneNodeType::Decal:
    case SceneNodeType::Camera:
        return false;
    case SceneNodeType::Empty:
    case SceneNodeType::Mesh:
        break;
    }
    if (isPhysicsBody) return false;         // Bullet writes its transform every step
    if (isSocketAttached()) return false;    // the socket resolver writes it every frame
    // AVATAR WRAPPERS (REALTIME_REFLECTIONS_SPEC §2, "Gap"): the movement
    // component and the locomotion state machine write this node's transform
    // every frame of play, and until 2026-09-12 nothing here said so — an
    // avatar was marked static by the default policy and only demoted by rule 4
    // once it had already taken a step.
    if (hasAvatarComponent()) return false;
    // ANIMATED means "something writes this node's transform", not "an
    // Animation object is attached". The distinction is not academic: the
    // animation panel gives every node it is shown a default, CHANNEL-LESS
    // `Animation` the moment the user looks at it, the writer persists it, and
    // the reader hands it straight back — so the shipped samples carry an empty
    // "Animation" on most of their TOP-LEVEL nodes (Showroom: 31 of 32 have no
    // properties and no skeletal clip at all).
    //
    // Counting those as animated cost every loaded world its ENTIRE static
    // classification (measured 2026-09-06: a freshly opened Showroom reported 0
    // static nodes out of 241, while the same scene's Add-menu additions
    // classified fine). Rule 2 is what turns it into a wipe-out rather than a
    // few misses: an ineligible top-level node stays dynamic, and
    // `canBeStatic` then refuses its whole subtree — 205 of Showroom's 241
    // nodes were themselves perfectly eligible and were refused for their
    // parent's sake.
    //
    // A channel-less animation drives nothing: SceneNode::updateAnimation only
    // writes a transform through `hasPropertyAnim("position"/"rotation"/
    // "scale")`, and a skeletal clip is the engine's. So the test is whether
    // any attached animation actually HAS something to play — and if one grows
    // channels later, rule 4 (the first transform write demotes the subtree)
    // catches it without a document-side hook.
    // A skeletal clip is the same question (BISTRO-IMPORT-1): it writes the nodes it
    // KEYS, wherever it is carried, and never its carrier merely for carrying it.
    if (drivesTransform(animation)) return false;
    for (const AnimationPtr &a : animations)
        if (drivesTransform(a)) return false;
    if (isKeyedByClip()) return false;
    return true;
}

void SceneNode::setMobility(Mobility m)
{
    // The user's word, recorded BEFORE anything is applied: a setting that
    // cannot hold today ("static" on a physics body) is still an opinion the
    // file should carry, and it becomes the answer the moment the body is
    // removed. resolveMobility() reports the disagreement openly rather than
    // silently dropping it.
    mMobility = m;
    // THE WHOLE SUBTREE RE-CLASSIFIES, not just this node. Rule 2 says a child
    // travels with its parent, so pinning a parent Movable changes what every
    // node under it resolves to — and if their stale `mStaticHint` is left
    // standing, the next scene bind replays it (reapplyStaticHints) and the
    // graph refuses each one under the now-dynamic parent: a warning per child,
    // per bind, for a classification the document already knows is wrong.
    //
    // applyStaticDefaults resolves this node's parent chain once and threads
    // its own answers down, which is exactly the pass an authoring change to a
    // driver needs. It records no user decision of its own (only this
    // function's `mMobility` write is the decision).
    applyStaticDefaults();
    // ...and the mark follows the classification: every node under this one
    // resolves differently now (DIRTY_SET_MIRROR_SPEC §3.4, the mobility row).
    notifyChangedSubtree(NodeChange::Flags);
}

void SceneNode::_applyStaticHint(bool value)
{
    if (value && !isStaticEligible()) {
        qWarning("iris::SceneNode: static graph class refused for '%s': this node kind moves "
                 "(SCENEGRAPH_SPEC §6 — lights, particles, decals, cameras, viewers, physics "
                 "bodies, socket riders, avatars and animated nodes are never graph-static).",
                 qPrintable(name));
        return;
    }
    // The GRAPH state is part of the test, not just the field: a node that
    // inherited static from a static parent has `mStaticHint == false` while
    // sitting in the static manager, and _applyStaticHint(false) on it must
    // really demote it.
    if (mStaticHint == value && graph::isStatic(mDoc) == value) return;
    mStaticHint = value;
    // The graph may refuse (an ineligible parent, an attachment that cannot
    // switch): the document then remembers what was asked for, because a later
    // reparent under a static parent makes the same request legal. `staticHint`
    // is the intent; `isStaticInGraph()` is the outcome.
    graph::setStatic(mDoc, value);
    notifyChanged(NodeChange::Flags);
}

void SceneNode::reapplyStaticHints()
{
    // Top-down: a parent must be static before its child asks (rule 2), and
    // Ogre pushes the class down anyway — the order makes every ask legal.
    //
    // `canBeStatic` FIRST (2026-09-12): this replays a hint recorded earlier,
    // and the world may have moved on — a parent that has since become movable
    // makes every stale hint under it illegal, and asking anyway costs a
    // qWarning per node per bind for something the document is not even
    // claiming any more. Asking only where the graph can say yes changes what
    // is re-asserted not at all: the refused asks were no-ops with a log line.
    if (mStaticHint && graph::canBeStatic(mDoc)) _applyStaticHint(true);
    const int n = childCount();
    for (int i = 0; i < n; ++i)
        if (SceneNode *c = childAt(i)) c->reapplyStaticHints();
}

void SceneNode::applyStaticDefaults()
{
    bool parentMovable = false;
    if (const SceneNode *p = mDoc.parent ? mDoc.parent->owner : nullptr)
        parentMovable = p->resolvedMobility() == Mobility::Movable;
    applyStaticDefaultsFrom(parentMovable);
}

namespace {
/// THE ONE RULE, recursed: the load's pass is the settle's step applied
/// top-down in one go (the mirror's settle runs the same step in slices).
void applyStaticDefaultsRec(SceneNode *node, bool parentMovable)
{
    const bool movable = node->_applyStaticDefaultsStep(parentMovable);
    // Descend regardless — a light in the middle of an imported rig does not
    // stop the props below it from being static, it only stops ITSELF (and,
    // through canBeStatic, the branch under it, which is the rule).
    for (const graph::DocNode *c : node->docNode().children)
        applyStaticDefaultsRec(c->owner, movable);
}
}

void SceneNode::applyStaticDefaultsFrom(bool parentMovable)
{
    applyStaticDefaultsRec(this, parentMovable);
}

bool SceneNode::_applyStaticDefaultsStep(bool parentMovable)
{
    // ONE NODE OF THE RULE. ONE RESOLUTION, TOP-DOWN: the parent's answer is
    // threaded down rather than re-walked per node, so a walk is O(nodes) and
    // not O(nodes x depth). The promotion is graph::promoteNode (this node
    // only) — the hint bookkeeping is _applyStaticHint's.
    const bool movable = resolveMobility(parentMovable) == Mobility::Movable;
    if (movable) {
        // Movable: out of the static half. `_applyStaticHint`, not setMobility
        // — the POLICY must never leave "the user asked for this" behind (that
        // would write a derivation into the file on the next save). A demotion
        // stays the subtree switch (promoteNode's note says why).
        _applyStaticHint(false);
    } else if (isStaticEligible() && graph::canBeStatic(mDoc)) {
        // canBeStatic() first: an ineligible PARENT means this whole branch
        // stays dynamic, and asking anyway would log a refusal per node.
        // _applyStaticHint(true)'s early return: hint set AND in the graph.
        const bool already = mStaticHint && graph::isStatic(mDoc);
        graph::promoteNode(mDoc, nullptr);
        if (!already) {
            mStaticHint = true;     // recorded even where the graph refused
            notifyChanged(NodeChange::Flags);
        }
    }
    return movable;
}

void SceneNode::addAnimation(AnimationPtr anim)
{
    animations.append(anim);
    // AN ANIMATION IS A MOBILITY DRIVER (resolveMobility rule 1) and takes the
    // node out of the static half (isStaticEligible), so attaching or dropping
    // one changes what the renderer is told about it. Flags, and over the
    // subtree, because rule 2 makes a child travel with its parent.
    notifyChangedSubtree(NodeChange::Flags);
}

QList<AnimationPtr> SceneNode::getAnimations()
{
    return animations;
}

void SceneNode::setAnimation(AnimationPtr anim)
{
    if (animation == anim) return;
    animation = anim;
    notifyChangedSubtree(NodeChange::Flags);
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
    notifyChangedSubtree(NodeChange::Flags);
}

void SceneNode::deleteAnimation(AnimationPtr anim)
{
    animations.removeOne(anim);
    notifyChangedSubtree(NodeChange::Flags);
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

    // Probe-grid placement exclusion (REFLECTIONS_ADOPTION_SPEC.md P1a.2) —
    // beside the other per-object rendering flags, reached from scripts as
    // node.setProperty(id, "probeGridExcluded", true).
    boolProp = new BoolProperty();
    boolProp->displayName = "Exclude From Probe Placement";
    boolProp->name = "probeGridExcluded";
    boolProp->value = probeGridExcluded;
    props.append(boolProp);

    boolProp = new BoolProperty();
    boolProp->displayName = "Pickable";
    boolProp->name = "pickable";
    boolProp->value = pickable;
    props.append(boolProp);

    // PRESERVE THE SCALE RATIO (SCALE-LOCK-1) — the transform panel's lock
    // icon, reached from a script as node.setScaleLock(id, true) or
    // node.setProperty(id, "scaleLock", true).
    boolProp = new BoolProperty();
    boolProp->displayName = "Lock Scale Ratio";
    boolProp->name = "scaleLock";
    boolProp->value = scaleLock;
    props.append(boolProp);

    // LIGHTING CHANNELS as a plain 32-bit row.
    //
    // THE ROW IS SIGNED AND THAT IS THE POINT: `int(0xFFFFFFFF)` is -1, so the
    // default reads as -1 = "every channel", which is exactly what Unity's
    // culling mask has spelled for fifteen years. The alternative (slicing the
    // byte the checkboxes edit into its own row) would make node.property and
    // node.properties disagree about what "lightMask" means, and would give a
    // script no way to reach the upper 24 bits at all. IntProperty cannot hold
    // an unsigned 32-bit value; the bit pattern is preserved exactly either
    // way, and `node.lightMask` / `node.setLightMask` are the unsigned
    // spelling for callers who prefer it.
    //
    // min/max stay 0/0, which the reflection layer reports as UNBOUNDED
    // (nodeapi's row doc) — a bitmask has no meaningful slider range.
    auto intProp = new IntProperty();
    intProp->displayName = "Lighting Channels";
    intProp->name = "lightMask";
    intProp->value = static_cast<int>(lightMask);
    props.append(intProp);

    // MOBILITY (REALTIME_REFLECTIONS_SPEC §3.3). An ENUM row on an IntProperty,
    // exactly like a mesh's faceCullingMode: there is no enum Property type,
    // and the verb surface maps the ordinal to a NAME at the boundary
    // (nodeapi's mobilityRowName). The row carries the SETTING — auto/static/
    // movable — never the resolved answer, because the resolution is derived
    // and a derived value in a writable row would be a setting that silently
    // rewrites itself.
    intProp = new IntProperty();
    intProp->displayName = "Movement";
    intProp->name = "mobility";
    intProp->value = static_cast<int>(mMobility);
    props.append(intProp);

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
    if (valueName == "probeGridExcluded") return getProbeGridExcluded();
    if (valueName == "pickable")   return isPickable();
    if (valueName == "scaleLock")  return getScaleLock();
    // Signed, matching the row above: -1 is "all channels".
    if (valueName == "lightMask")  return static_cast<int>(lightMask);
    if (valueName == "mobility")   return static_cast<int>(mMobility);

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
    if (valueName == "probeGridExcluded") { setProbeGridExcluded(value.toBool());   return true; }
    if (valueName == "pickable")   { setPickable(value.toBool());                return true; }
    if (valueName == "scaleLock")  { setScaleLock(value.toBool());               return true; }
    // Accepts BOTH spellings of the same 32 bits: -1 (the signed row this node
    // reflects) and 4294967295 (what an unsigned-minded caller will send).
    // toInt() alone would turn the latter into 0 with ok=false — i.e. "no
    // channels at all" — which is the exact opposite of what was asked for, so
    // the wide read is not a nicety.
    if (valueName == "lightMask") {
        bool ok = false;
        const qlonglong wide = value.toLongLong(&ok);
        if (!ok) return false;
        setLightMask(static_cast<quint32>(wide & 0xFFFFFFFFll));
        return true;
    }
    if (valueName == "mobility") {
        bool ok = false;
        const int m = value.toInt(&ok);
        if (!ok || m < int(Mobility::Auto) || m > int(Mobility::Movable)) return false;
        setMobility(static_cast<Mobility>(m));
        return true;
    }
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
    // The world POSE, not a matrix: the re-expression below inverts Ogre's own
    // composition rule exactly (graph::writeGlobalTrs) — a matrix would be
    // decomposed again, losing a negative scale's sign convention and any shear.
    const graph::Trs initialWorld = graph::world(node->mDoc);

    // A move inside the same scene keeps its scene membership: tearing the
    // registries down and rebuilding them loses state that only exists at the
    // scene level (Scene::removeNode clears activeCameraGuid — a reparented
    // active camera silently stopped being active) and pays remove+add over
    // the whole subtree for a node that never left the document.
    const bool sameSceneMove = node->hasScene() && getScene() &&
                               node->getScene() == getScene();
    if (auto oldParent = node->getParent())
        oldParent->removeChildInternal(node, sameSceneMove);

    // THE STORE: the document's hierarchy (graph::DocNode); its mirror tells the
    // engine the new parent (the frame record).
    graph::attachDoc(mDoc, node->mDoc, position);

    mChildRefs.append(node);      // the lifetime anchor; never the structure

    if (auto sc = getScene()) {
        if (!node->hasScene()) node->setScene(sc);
    }

    if (keepTransform) {
        // The world pose the node had before the move, re-expressed in the new
        // parent's space by the exact inverse of the composition rule (exact for
        // any parent chain, a non-uniform or a zero scale included —
        // SC1-REEXPRESS-SCALE-1: the matrix inverse + decomposition this
        // replaced was exact only under uniform scale).
        //
        // A RE-EXPRESSION, NOT A MOVE (nodegraph.h rule 4, ReexpressScope): the
        // world pose is preserved by construction, so the write keeps the node's
        // class — Ogre's static pass re-derives the subtree once. (It used to fire
        // rule 4's demotion and re-promote right after: 2 x the subtree's nodes in
        // class switches per reparent.) The hint below still re-asserts the class
        // for a node attach() had to demote under a dynamic parent — eligibility
        // is validated under the NEW parent.
        const bool wantedStatic = node->wantsStatic();
        {
            graph::ReexpressScope reexpress;
            node->notifyChanged(NodeChange::Transform);
            graph::writeGlobalTrs(node->mDoc, initialWorld);
        }
        // _applyStaticHint, not setMobility: a plain reparent of a
        // policy-derived static node must not stamp a USER decision (which the
        // serializer persists) into the file.
        if (wantedStatic) node->_applyStaticHint(true);
    }

    // THE WHOLE MOVED SUBTREE: inherited visibility and mobility come from the
    // new parent, so every descendant owes a re-resolve (§3.4). A fragment
    // that just joined a scene marks here for the first time — its nodes had
    // no collector until setScene above gave them one.
    node->notifyChangedSubtree(NodeChange::Structure);
    notifyChanged(NodeChange::Structure);
}

void SceneNode::removeFromParent()
{
    auto self = sharedFromThis();
    if (auto p = getParent()) p->removeChild(self);
}

void SceneNode::removeChild(SceneNodePtr node)
{
    removeChildInternal(node);
}

void SceneNode::removeChildInternal(const SceneNodePtr &node, bool keepSceneMembership)
{
    if (!node) return;
    // Out of the tree first (this is what makes it stop rendering and stop
    // being reachable), THEN out of the scene registries, THEN drop our
    // ownership — the caller's own SceneNodePtr is what keeps it alive.
    graph::detachDoc(node->mDoc);
    // keepSceneMembership: insertChild's same-scene move — the node is about
    // to be re-attached under a parent in the SAME scene, so registries stay.
    if (!keepSceneMembership) node->removeFromScene();
    mChildRefs.removeOne(node);
    // A SAME-SCENE REPARENT keeps its membership (and its collector): the
    // subtree is marked by the insertChild that follows. A real removal has
    // already been recorded as an EVICTION by removeFromScene above, and the
    // mark below would be dropped on the floor with the node's collector.
    node->notifyChangedSubtree(NodeChange::Structure);
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
    // THE CHANGE COLLECTOR travels with scene membership, and only with it: a
    // node outside a scene records nothing (there is no mirror that could ever
    // be asked about it), and joining one is a Structure event that marks the
    // whole subtree below.
    _setDirtySet(scene->dirtySet());
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
    // OUT OF THE DOCUMENT: the mirror still holds an entry keyed by this
    // pointer and has to release it. The pointer is RECORDED, never followed —
    // releaseEntry reads the entry alone — and it is processed before the next
    // frame's dirty list so an address a same-frame insert recycles is freed
    // before it is adopted again (DIRTY_SET_MIRROR_SPEC §3.4).
    _leaveDirtySet();
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
    graph::writeGlobalPos(mDoc, pos);
}

void SceneNode::setGlobalRot(iris::Quat rot)
{
    notifyChanged(NodeChange::Transform);
    graph::writeGlobalRot(mDoc, rot);
}

void SceneNode::setGlobalPosRot(iris::Vec3 pos, iris::Quat rot)
{
    // ONE notification and ONE parent resolution for a pose that arrives whole
    // — the physics write-back's shape (MIRROR_SCALE lane).
    notifyChanged(NodeChange::Transform);
    graph::writeGlobalPosRot(mDoc, pos, rot);
}

void SceneNode::setGlobalTransform(iris::Mat4 transform)
{
    notifyChanged(NodeChange::Transform);
    graph::writeGlobalTransform(mDoc, transform);
}

SceneNodePtr SceneNode::duplicate()
{
    QHash<QString, QString> guidMap;
    auto node = duplicateInto(guidMap);
    if (!node) return node;
    node->remapNodeReferences(guidMap);
    return node;
}

void SceneNode::remapNodeReferences(const QHash<QString, QString> &guidMap)
{
    const auto through = [&](QString &field) {
        if (field.isEmpty()) return;
        const auto it = guidMap.constFind(field);
        if (it != guidMap.constEnd()) field = it.value();
    };

    // A SOCKET RIDER: copying a character with a camera on its head must give
    // the COPY's camera the COPY's head. An owner OUTSIDE the copied subtree
    // keeps its guid — the "second camera on the same character" case.
    through(socketOwnerGuid);

    // PHYSICS CONSTRAINT ENDPOINTS (CLIPBOARD_SPEC §3.2, the recorded gap).
    // Both ends name nodes by guid, and until now neither Duplicate nor Paste
    // re-pointed them: a duplicated pair of constrained bodies stayed bolted to
    // the ORIGINALS, so dragging the copy dragged the original's rig. Same rule
    // as the socket owner — an endpoint outside the copy is deliberately left
    // alone, because a constraint to a fixed anchor is a real authoring shape.
    for (auto &constraint : physicsProperty.constraints) {
        through(constraint.constraintFrom);
        through(constraint.constraintTo);
    }

    // Per-type references (a camera's focus target). Virtual rather than a
    // dynamic_cast ladder here: this file must not know the subclasses, and a
    // new node type with a node-guid field then cannot forget to be listed —
    // it overrides one function beside the field it added.
    remapOwnNodeReferences(guidMap);

    const int n = childCount();
    for (int i = 0; i < n; ++i) if (SceneNode *c = childAt(i)) c->remapNodeReferences(guidMap);
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
	// THE LOCK TRAVELS WITH THE COPY (SCALE-LOCK-1): a duplicate of a node
	// whose proportions you are keeping is a node whose proportions you are
	// keeping.
	node->scaleLock		= this->scaleLock;
	node->planarReflector = this->planarReflector;
	node->probeGridExcluded = this->probeGridExcluded;
	node->lightMask		= this->lightMask;
	node->attached		= this->attached;
	// PHYSICS travels with the copy (platform audit B5.2): a duplicate of a rigid
	// body is a rigid body with the same shape, mass and constraints — the
	// constraint remap in remapNodeReferences exists for exactly this case and
	// was dead on this path while the copy came back with no physics at all.
	node->isPhysicsBody  = this->isPhysicsBody;
	node->physicsProperty = this->physicsProperty;
	// Whether a character can walk into the copy (AVATAR_LOCOMOTION_SPEC §6.3).
	// The constructor already set the TYPE default; this carries the user's
	// override, so duplicating a mesh you had made non-solid does not hand back
	// a solid one.
	node->collisionEnabled = this->collisionEnabled;
	// THE AVATAR COMPONENT, DEEP-COPIED. Sharing the pointer would put two
	// scene nodes on one movement component: both would be stepped, each would
	// overwrite the other's velocity and jump counter, and only one of them
	// would appear to move. A duplicate of a character is a SECOND character.
	if (this->avatarMovement) {
		auto copy = AvatarMovementPtr(new AvatarMovement());
		copy->setParams(this->avatarMovement->params());
		node->setAvatarComponent(copy);
	}
	// THE AVATAR LINK travels with the copy: duplicating an instance of an
	// avatar asset gives you a SECOND INSTANCE of the same asset, not an
	// orphan. (Instances share the project's version — the per-instance
	// override is a later stage, AVATAR_ASSET_SPEC §11.)
	node->avatarLink = this->avatarLink;
	// THE LOCOMOTION STATE MACHINE, deep-copied for the same reason: the asset
	// and the role bindings travel with the copy (they are the character's
	// authoring), but the CLOCK does not — a duplicate starts at its entry
	// state with a zero phase rather than mid-stride at the original's phase,
	// which is what stops two copies of one character from marching in
	// lock-step (§3.2's per-avatar phase requirement, from the other end).
	if (this->avatarLocomotion) {
		auto copy = AvatarLocomotionPtr(new AvatarLocomotion());
		copy->markRolesFromFile(this->avatarLocomotion->roles(),
		                        this->avatarLocomotion->usesDefaultAsset());
		QString err;
		copy->setAssetPreservingDefaultFlag(this->avatarLocomotion->asset(), &err);
		node->setLocomotionComponent(copy);
	}
    // The user's MOBILITY decision travels with the copy (the derived graph
    // hint does not: the copy is about to be parented somewhere, and the
    // classification pass that follows every add re-derives it). Without this a
    // duplicate of a node the user had pinned Movable came back static on the
    // next load.
    node->_setMobility(this->mMobility);
	// A duplicate lands in the same outliner folder as its original — a copy
	// that jumped back to the root level would be a small, constant annoyance
	// (SCENEGRAPH_SPEC §6b). The folder itself is untouched; this is metadata.
	node->folderPath	= this->folderPath;

	// THE ANIMATIONS travel with the copy (CLIP-REF-1): a duplicated avatar
	// that came back with no clips stood in its bind pose. Each animation is
	// cloned (own property keys, the SAME skeletal clip and its reference),
	// and the copy plays the clip the original plays.
	for (const auto &anim : this->animations) {
		if (anim.isNull()) continue;
		auto copy = anim->clone();
		node->addAnimation(copy);
		if (anim == this->animation) node->setAnimation(copy);
	}

    auto id = QUuid::createUuid();
    auto guid = id.toString().remove(0, 1);
    guid.chop(1);
    node->setGUID(guid);
    guidMap.insert(this->getGUID(), guid);

    // The attachment travels with the copy. Whether it points at the ORIGINAL
    // owner or at the copy's own is decided by remapNodeReferences once the whole
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
