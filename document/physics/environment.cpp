#include "core/math/mat4.h"
#include "core/math/vec.h"
#include "document/physics/environment.h"

#include "btBulletDynamicsCommon.h"
#include "BulletCollision/CollisionDispatch/btGhostObject.h"
#include "BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h"

#include "document/physics/avatarmovement.h"
#include "document/animation/locomotion.h"
#include "document/scenegraph/meshnode.h"
#include "document/scenegraph/scene.h"
#include "document/scenegraph/scenenode.h"
#include "document/assets/mesh.h"
#include "core/geometry/trimesh.h"

namespace iris
{

Environment::Environment()
{
	worldYGravity = 15.f;
    createPhysicsWorld();
 
    simulating = false;

	//activePickingConstraint = 0;
	pickingHandles[(int)PickingHandleType::LeftHand] = PickingHandle();
	pickingHandles[(int)PickingHandleType::RightHand] = PickingHandle();
	pickingHandles[(int)PickingHandleType::MouseButton] = PickingHandle();
}

Environment::~Environment()
{
    destroyPhysicsWorld();
}

void Environment::addBodyToWorld(btRigidBody *body, const iris::SceneNodePtr &node)
{
    world->addRigidBody(body);

	hashBodies.insert(node->getGUID(), body);
	nodeTransforms.insert(node->getGUID(), node->getGlobalTransform());
}

void Environment::addBodyToWorld(PhysicsBody &owned, const iris::SceneNodePtr &node)
{
	if (!owned.body) return;
	addBodyToWorld(owned.body, node);
	// The world takes over everything the helper allocated. Bullet reference
	// counts none of it (deep audit 2026-09, area 4 F3).
	for (auto *shape : owned.shapes) collisionShapes.push_back(shape);
	for (auto *iface : owned.meshInterfaces) meshInterfaces.append(iface);
	owned.shapes.clear();
	owned.meshInterfaces.clear();
	owned.body = nullptr;
}

void Environment::removeBodyFromWorld(btRigidBody *body)
{
    // The key has to be read BEFORE the first remove(): the old code looked it
    // up again for nodeTransforms, by which time the hash no longer held the
    // body and key() returned a default-constructed QString — so the node
    // transform was never dropped.
    const QString guid = hashBodies.key(body);
    if (!hashBodies.contains(guid)) return;

    world->removeRigidBody(body);
    hashBodies.remove(guid);
    nodeTransforms.remove(guid);
}

void Environment::removeBodyFromWorld(const QString &guid)
{
    if (!hashBodies.contains(guid)) return;

    world->removeRigidBody(hashBodies.value(guid));
    hashBodies.remove(guid);
    nodeTransforms.remove(guid);
}

void Environment::storeCollisionShape(btCollisionShape *shape)
{
    if (shape) collisionShapes.push_back(shape);
}

void Environment::storeMeshInterface(btStridingMeshInterface *iface)
{
    if (iface) meshInterfaces.append(iface);
}

void Environment::addConstraintToWorld(btTypedConstraint *constraint, bool disableCollisions)
{
    // createConstraintFromProperty returns null for a constraint kind it does
    // not build; bullet dereferences what it is handed.
    if (!constraint || !world) return;
    world->addConstraint(constraint, disableCollisions);
    constraints.append(constraint);
}

void Environment::removeConstraintFromWorld(btTypedConstraint *constraint)
{
    for (int i = 0; i < constraints.size(); ++i) {
        if (constraints[i] == constraint) {
            constraints.erase(constraints.begin() + i);
            world->removeConstraint(constraint);
            break;
        }
    }
}

void Environment::initializePhysicsWorldFromScene(const iris::SceneNodePtr rootNode)
{
	std::function<void(const SceneNodePtr)> createPhysicsBodiesFromNode = [&](const SceneNodePtr node) {
		for (const auto child : node->children()) {
			if (child->isPhysicsBody) {
				auto owned = PhysicsHelper::createPhysicsBody(child, child->physicsProperty);
				// The overload that ALSO takes the shapes: without it every
				// shape, compound child and triangle-mesh interface built here
				// leaked, once per Play.
				addBodyToWorld(owned, child);
			}

			createPhysicsBodiesFromNode(child);
		}
	};

	createPhysicsBodiesFromNode(rootNode);

	// AVATARS, in DOCUMENT ORDER (depth first from the root) — the order §8.5's
	// "auto-possess the first avatar in the scene" is defined against, and the
	// order updateAvatarMovement steps them in.
	std::function<void(const SceneNodePtr &)> registerAvatars = [&](const SceneNodePtr &node) {
		const int kids = node->childCount();
		for (int i = 0; i < kids; ++i) {
			iris::SceneNode *raw = node->childAt(i);
			if (!raw) continue;
			auto child = raw->sharedFromThis();
			if (child->hasAvatarComponent()) addAvatarToWorld(child);
			registerAvatars(child);
		}
	};
	registerAvatars(rootNode);

	// §6.3 option C, and its cost fence: this returns immediately when the
	// scene has no avatar in it, so a scene that never had a character costs
	// exactly what it cost before this feature existed.
	buildCollisionContent(rootNode);

	// now add constraints
	// TODO - avoid looping like this, get constraint list -- list and then use that
	// TODO - handle children of children?
	for (const auto &node : rootNode->children()) {
		if (node->isPhysicsBody) {
			for (const auto &constraintProperties : node->physicsProperty.constraints) {
				auto constraint = PhysicsHelper::createConstraintFromProperty(this, constraintProperties);
				addConstraintToWorld(constraint);
			}
		}
	}

	// notice the - sign for the gravity, show it as positive in the interface but flip it here
	world->setGravity(btVector3(0, -worldYGravity, 0));
}

btDynamicsWorld *Environment::getWorld()
{
    return world;
}

void Environment::simulatePhysics()
{
    simulating = true;
    simulationStarted = true;
}

bool Environment::isSimulating()
{
    return simulationStarted;
}

void Environment::stopPhysics()
{
    // this is the original, we also want to be able to pause as well
    // to "restart" a sim we have to cleanup and recreate it from scratch basically...
	//simulating = false;
	simulating = false;
}

void Environment::stopSimulation()
{
    simulationStarted = false;
}

void Environment::stepSimulation(float delta)
{
    if (simulating) {
		world->stepSimulation(delta);
		// AVATAR_LOCOMOTION_SPEC §6.3: the seam the 2016 controller update
		// left. AFTER the rigid-body solve, so the sweeps see this frame's
		// world, and OUTSIDE bullet's substepping, because the movement
		// component does its own fixed sub-stepping (§6.1) and a component
		// stepped from a bullet internal tick would be stepped a variable
		// number of times per frame — defect 3 of the removed controller.
		updateAvatarMovement(delta);
		//drawDebugShapes();
    }
}

// ---------------------------------------------------------------------------
// AVATARS (AVATAR_LOCOMOTION_SPEC §6)

void Environment::addAvatarToWorld(const iris::SceneNodePtr &node)
{
	if (!node || !node->hasAvatarComponent()) return;
	const QString guid = node->getGUID();
	for (const auto &weak : avatars) {
		auto existing = weak.toStrongRef();
		if (existing && existing->getGUID() == guid) return;
	}
	avatars.append(node.toWeakRef());
	// PRE-PLAY TRANSFORM, so Stop puts the character back where the user left
	// it. An avatar is NOT a physics body, so it never appeared in this hash
	// and `restoreNodeTransformations` would have left it wherever it walked to
	// — the spec's acceptance step 5 ("Stop returns both to their pre-play
	// transforms") read as a defect.
	nodeTransforms.insert(guid, node->getGlobalTransform());
	// R6, answered: an avatar spawned DURING play registers on the spot rather
	// than being refused. Its capsule is fitted here so a spawn that happens
	// before the mesh finished loading still gets the right dimensions.
	if (auto *movement = node->avatar()) {
		movement->fitCapsuleToNode(node);
		movement->reset();
	}
	// A late arrival needs something to stand on: content built lazily at play
	// start would otherwise be missing for the first avatar spawned after it.
	if (auto scene = node->getScene())
		buildCollisionContent(scene->getRootNode());
}

void Environment::removeAvatarFromWorld(const QString &guid)
{
	for (int i = avatars.size() - 1; i >= 0; --i) {
		auto node = avatars[i].toStrongRef();
		if (!node || node->getGUID() == guid) avatars.removeAt(i);
	}
}

void Environment::removeAllAvatarsFromWorld()
{
	avatars.clear();
}

void Environment::updateAvatarMovement(float delta)
{
	// PRUNE first, in one backward pass. An avatar deleted mid-play (gate M7)
	// leaves an expired weak pointer, and a node whose scene is gone has been
	// taken out of the document — either way the world must stop stepping it,
	// and it must stop stepping it BEFORE the step loop so the loop can run
	// forward.
	for (int i = avatars.size() - 1; i >= 0; --i) {
		auto node = avatars[i].toStrongRef();
		// The SCENE'S REGISTRY is the truth for "still in the document", not the
		// shared pointer: `Scene::removeNode` drops the guid from `nodes` but
		// does NOT clear the node's own scene back-pointer, and the undo stack
		// deliberately keeps a deleted node alive (SCENEGRAPH audit §3.3). So a
		// deleted-but-undoable avatar has a live pointer AND a live scene
		// pointer, and only its absence from the registry says it is gone.
		auto scene = node ? node->getScene() : iris::ScenePtr();
		const bool gone = !node || !node->hasAvatarComponent() || !scene
		                  || !scene->nodes.contains(node->getGUID());
		if (gone) avatars.removeAt(i);
	}

	// EVERY avatar, possessed or not, in REGISTRATION ORDER, and no `break`:
	// the removed controller iterated a QHash and stopped at the first active
	// entry, so with two characters the one that walked was whichever the hash
	// happened to order first (§4.4 defect 1). Registration order is document
	// order, which is what makes a two-avatar run reproducible.
	for (int i = 0; i < avatars.size(); ++i) {
		auto node = avatars[i].toStrongRef();
		if (!node) continue;
		AvatarMovement *movement = node->avatar();
		movement->step(world, node, delta, world ? float(world->getGravity().y()) : -10.0f);
		// THE LOCOMOTION STATE MACHINE, stepped IMMEDIATELY after the movement
		// that published the contract it reads (AVATAR_LOCOMOTION_SPEC §7,
		// Stage 4). Immediately, and not a frame later, because
		// `jumpRequested` is a LATCH: it is true only in the step the movement
		// component consumed it, so a machine stepped on the next frame would
		// never see a jump and §7.2's transition 1 would never fire.
		if (AvatarLocomotion *loco = node->locomotion())
			loco->stepForNode(node, movement->state(), delta,
			                  movement->params().walkSpeed, movement->params().runSpeed);
	}
}

void Environment::buildCollisionContent(const iris::SceneNodePtr &rootNode)
{
	if (!rootNode || !world) return;
	// LAZY, and this is the whole cost story (§6.3's objection to option B):
	// no avatar in the scene means no character can walk into anything, so
	// nothing is built and Play costs exactly what it costs today.
	if (avatars.isEmpty()) return;

	std::function<void(const iris::SceneNodePtr &)> walk = [&](const iris::SceneNodePtr &node) {
		const int kids = node->childCount();
		for (int i = 0; i < kids; ++i) {
			iris::SceneNode *raw = node->childAt(i);
			if (!raw) continue;
			auto child = raw->sharedFromThis();
			// A node that is ALREADY a physics body has a collider; giving it a
			// second one would make the character collide with the box twice.
			// An avatar's own subtree is skipped too — a character must not be
			// a wall to itself.
			const bool skip = child->isPhysicsBody || child->hasAvatarComponent();
			if (!skip && child->getSceneNodeType() == iris::SceneNodeType::Mesh
			    && child->isCollisionEnabled()
			    && !collisionContentNodes.contains(child->getGUID())) {
				auto meshNode = child.staticCast<iris::MeshNode>();
				auto mesh = meshNode->getMesh();
				if (mesh && mesh->getTriMesh() && !mesh->getTriMesh()->triangles.isEmpty()) {
					btTriangleMesh *tri = iris::PhysicsHelper::btTriangleMeshShapeFromMesh(mesh);
					// A BVH shape, built once: this is static content and the
					// sweep is the only thing that ever queries it.
					auto *shape = new btBvhTriangleMeshShape(tri, true);
					// The GLOBAL scale, read off the transform's basis columns:
					// the document exposes only a LOCAL scale, and using it
					// here would silently ignore every scaled parent — the
					// class of bug where the collider is a tenth the size of
					// the wall you can see.
					const auto xform = child->getGlobalTransform();
					const float sx = iris::Vec3(xform(0, 0), xform(1, 0), xform(2, 0)).length();
					const float sy = iris::Vec3(xform(0, 1), xform(1, 1), xform(2, 1)).length();
					const float sz = iris::Vec3(xform(0, 2), xform(1, 2), xform(2, 2)).length();
					shape->setLocalScaling(btVector3(sx, sy, sz));
					btTransform t;
					t.setIdentity();
					const auto pos = child->getGlobalPosition();
					const auto rot = child->getGlobalRotation();
					t.setOrigin(btVector3(pos.x(), pos.y(), pos.z()));
					t.setRotation(btQuaternion(rot.x(), rot.y(), rot.z(), rot.scalar()));
					auto *obj = new btCollisionObject();
					obj->setCollisionShape(shape);
					obj->setWorldTransform(t);
					obj->setCollisionFlags(obj->getCollisionFlags()
					                       | btCollisionObject::CF_STATIC_OBJECT);
					world->addCollisionObject(obj);
					// The world owns all three now; destroyPhysicsWorld's
					// collision-object loop deletes the object, and the shape
					// and its triangle data ride the same owned arrays every
					// other body's do.
					collisionShapes.push_back(shape);
					meshInterfaces.append(tri);
					collisionObjects.insert(child->getGUID(), obj);
					collisionContentNodes.insert(child->getGUID());
				}
			}
			// The skip must cover the SUBTREE, not just the node: an avatar's
			// wrapper carries the component but its MESH is a child, and
			// descending here rebuilt that mesh as a static wall the capsule
			// starts inside — a spawned character at certain offsets could not
			// move at all (found by the Stage 3 lane, measured on rig2.glb).
			// A physics body's subtree likewise already moves with its body.
			if (!skip) walk(child);
		}
	};
	walk(rootNode);
}

void Environment::restoreNodeTransformations(iris::SceneNodePtr rootNode)
{
	// Null-tolerant (a scene switch can hand in a torn-down scene's null root
	// — crash-1788594910.log) and RECURSIVE: body creation walks the whole
	// subtree (initializePhysicsWorldFromScene), so restore must too — a
	// nested physics body never returned to its pre-play pose (deep-audit F4).
	if (rootNode) restoreNodeTransformationsRecursive(rootNode);

	nodeTransforms.clear();
	nodeTransforms.squeeze();
}

void Environment::restoreNodeTransformationsRecursive(const iris::SceneNodePtr &node)
{
	for (auto &child : node->children()) {
		// Physics bodies AND avatars: the movement component writes the wrapper
		// node's transform every frame, and nothing else would ever put it back
		// (AVATAR_LOCOMOTION_SPEC §2 step 5).
		const bool restorable = child->isPhysicsBody || child->hasAvatarComponent();
		if (restorable && nodeTransforms.contains(child->getGUID()))
			child->setGlobalTransform(nodeTransforms.value(child->getGUID()));
		restoreNodeTransformationsRecursive(child);
	}
}

void Environment::restartPhysics()
{
	// node transforms are reset inside button caller
	stopPhysics();
	stopSimulation();

	destroyPhysicsWorld();
	createPhysicsWorld();
}

void Environment::createPhysicsWorld()
{
	btVector3 worldMin(-1000, -1000, -1000);
	btVector3 worldMax(1000, 1000, 1000);
	btAxisSweep3* sweepBP = new btAxisSweep3(worldMin, worldMax);
	// The pair cache stores the callback but does not own it — one leaked per
	// play/stop cycle before this member.
	ghostPairCallback = new btGhostPairCallback();
	sweepBP->getOverlappingPairCache()->setInternalGhostPairCallback(ghostPairCallback);
	broadphase = sweepBP;

	collisionConfig = new btDefaultCollisionConfiguration();
	dispatcher = new btCollisionDispatcher(collisionConfig);
	solver = new btSequentialImpulseConstraintSolver();
	world = new btDiscreteDynamicsWorld(dispatcher, broadphase, solver, collisionConfig);

	hashBodies.reserve(512);
	nodeTransforms.reserve(512);

	world->setGravity(btVector3(0, -worldYGravity, 0));
	world->getDispatchInfo().m_allowedCcdPenetration = 0.0001f;
}

void Environment::createPickingConstraint(PickingHandleType handleType, const QString &pickedNodeGUID, const btVector3 &hitPoint, const iris::Vec3 &segStart, const iris::Vec3 &segEnd)
{
	PickingHandle& handle = pickingHandles[(int)handleType];

	// Fetch our rigid body from the list stored in the world by guid.
	// A miss is NORMAL, not exceptional: the caller guards on the DOCUMENT's
	// isPhysicsBody flag, but the WORLD may hold no body for that guid — the
	// simulation may not have built bodies yet, the body's creation may have
	// failed, or a restart may be mid-flight. Dereferencing the null return
	// was the owner's first captured crash-*.log (2026-09-05: clicking the
	// Showroom floor in play mode).
	handle.activeRigidBodyBeingManipulated = hashBodies.value(pickedNodeGUID);
	if (!handle.activeRigidBodyBeingManipulated) {
		irisLog("physics: no rigid body in the world for picked node " +
		        pickedNodeGUID + " — picking constraint refused");
		return;
	}
	// Prevent the picked object from falling asleep while it is being moved
	handle.activeRigidBodySavedState = handle.activeRigidBodyBeingManipulated->getActivationState();
	handle.activeRigidBodyBeingManipulated->setActivationState(DISABLE_DEACTIVATION);
	// Get the hit position relative to the body we hit 
	// Constraints MUST be defined in local space coords
	btVector3 localPivot = handle.activeRigidBodyBeingManipulated->getCenterOfMassTransform().inverse() * hitPoint;

	// Create a transform for the pivot point
	btTransform pivot;
	pivot.setIdentity();
	pivot.setOrigin(localPivot);

	// Create our constraint object
	auto dof6 = new btGeneric6DofConstraint(*handle.activeRigidBodyBeingManipulated, pivot, true);
	bool bLimitAngularMotion = true;
	if (bLimitAngularMotion) {
		dof6->setAngularLowerLimit(btVector3(0, 0, 0));
		dof6->setAngularUpperLimit(btVector3(0, 0, 0));
	}

	// Add the constraint to the world
	addConstraintToWorld(dof6, false);
	// Store a pointer to our constraint
	handle.activePickingConstraint = dof6;

	// Define the 'strength' of our constraint (each axis)
	float cfm = 0.0f;
	// Define the 'error reduction' of our constraint (each axis)
	float erp = 0.5f;

	for (int i = 0; i < 6; ++i) {
		dof6->setParam(BT_CONSTRAINT_STOP_CFM, cfm, i);
		dof6->setParam(BT_CONSTRAINT_STOP_ERP, erp, i);
	}

	btVector3 rayFromWorld = iris::PhysicsHelper::btVector3FromVec3(segStart);
	btVector3 rayToWorld = iris::PhysicsHelper::btVector3FromVec3(segEnd);

	handle.constraintOldPickingPosition = rayToWorld;
	handle.constraintHitPosition = hitPoint;
	handle.constraintOldPickingDistance = (hitPoint - rayFromWorld).length();
}

void Environment::updatePickingConstraint(PickingHandleType handleType, const btVector3 &rayDirection, const btVector3 &cameraPosition)
{
	PickingHandle& handle = pickingHandles[(int)handleType];

	if (handle.activeRigidBodyBeingManipulated && handle.activePickingConstraint) {
		btGeneric6DofConstraint* pickingConstraint = static_cast<btGeneric6DofConstraint*>(handle.activePickingConstraint);
		if (pickingConstraint) {
			// use another picking ray to get the target direction
			btVector3 dir = rayDirection;
			dir.normalize();
			// use the same distance as when we originally picked the object
			dir *= handle.constraintOldPickingDistance;
			btVector3 newPivot = cameraPosition + dir;
			// set the position of the constraint
			pickingConstraint->getFrameOffsetA().setOrigin(newPivot);
		}
	}
}

void Environment::updatePickingConstraint(PickingHandleType handleType, const iris::Mat4 &handTransformation)
{
	PickingHandle& handle = pickingHandles[(int)handleType];

	if (handle.activeRigidBodyBeingManipulated && handle.activePickingConstraint) {
		btGeneric6DofConstraint* pickingConstraint = static_cast<btGeneric6DofConstraint*>(handle.activePickingConstraint);
		if (pickingConstraint) {
			pickingConstraint->getFrameOffsetA().setIdentity();
			pickingConstraint->getFrameOffsetA().setFromOpenGLMatrix(handTransformation.constData());
		}
	}
}

void Environment::cleanupPickingConstraint(PickingHandleType handleType)
{
	PickingHandle& handle = pickingHandles[(int)handleType];

	if (handle.activePickingConstraint) {
		if (handle.activeRigidBodyBeingManipulated) {
			handle.activeRigidBodyBeingManipulated->forceActivationState(handle.activeRigidBodySavedState);
			handle.activeRigidBodyBeingManipulated->activate();
		}
		btTypedConstraint *constraint = handle.activePickingConstraint;
		removeConstraintFromWorld(constraint);
		handle.activePickingConstraint = nullptr;
		// The rigid body is NOT ours — the world owns it and destroyPhysicsWorld
		// deletes it with the rest of the collision objects. Only the picking
		// constraint we created here is. (The old code nulled both members
		// FIRST and then "deleted" the nulls: the constraint leaked on every
		// drag, and the intent to delete the body would have been a
		// double-free.)
		handle.activeRigidBodyBeingManipulated = nullptr;
		delete constraint;
	}
}

// Defunct, since the environment isn't dynamic anymore, properties are added when simulation starts still keep this around (iKlsR)
void Environment::createConstraintBetweenNodes(iris::SceneNodePtr node, const QString &to, const iris::PhysicsConstraintType &type)
{
	// Adds this constraint to two rigid bodies, the first is the currently selected node/body
	// The second is selected from a menu ... TODO - do an interactive pick for selecting the second node
	auto bodyA = hashBodies.value(node->getGUID());
	auto bodyB = hashBodies.value(to);

	// Constraints must be defined in LOCAL SPACE...
	btVector3 pivotA = bodyA->getCenterOfMassTransform().getOrigin();
	btVector3 pivotB = bodyB->getCenterOfMassTransform().getOrigin();

	// Prefer a transform instead of a vector ... the majority of constraints use transforms
	btTransform frameA;
	frameA.setIdentity();
	frameA.setOrigin(bodyA->getCenterOfMassTransform().inverse() * pivotA);

	btTransform frameB;
	frameB.setIdentity();
	frameB.setOrigin(bodyB->getCenterOfMassTransform().inverse() * pivotA);

	btTypedConstraint *constraint = Q_NULLPTR;

	iris::ConstraintProperty constraintProperty;
	constraintProperty.constraintFrom = node->getGUID();
	constraintProperty.constraintTo = to;

	if (type == iris::PhysicsConstraintType::Ball) {
		constraint = new btPoint2PointConstraint(
			*bodyA, *bodyB, frameA.getOrigin(), frameB.getOrigin()
		);

		constraintProperty.constraintType = iris::PhysicsConstraintType::Ball;
	}

	if (type == iris::PhysicsConstraintType::Dof6) {
		constraint = new btGeneric6DofConstraint(
			*bodyA, *bodyB, frameA, frameB, true
		);

		constraintProperty.constraintType = iris::PhysicsConstraintType::Dof6;
	}

	node->physicsProperty.constraints.push_back(constraintProperty);

	constraint->setDbgDrawSize(btScalar(6));

	//constraint->m_setting.m_damping = 1.f;
	//constraint->m_setting.m_impulseClamp = 1.f;

	// Add the constraint to the physics world
	addConstraintToWorld(constraint);
}

void Environment::setWorldGravity(btScalar gravity)
{
	worldYGravity = gravity;
}

float Environment::getWorldGravity()
{
	return worldYGravity;
}

void Environment::destroyPhysicsWorld()
{
	// this is rougly verbose the same thing as the exitPhysics() function in the bullet demos

	// The avatar registry holds no world object, but it must not outlive the
	// world it swept against: a component stepped after this would hand a
	// dangling btCollisionWorld* to convexSweepTest.
	avatars.clear();
	collisionContentNodes.clear();
	collisionObjects.clear();

	if (world) {
		int i;
		for (i = world->getNumConstraints() - 1; i >= 0; i--) {
			world->removeConstraint(world->getConstraint(i));
		}

		// removeConstraint() only unregisters. The constraints WE created
		// (addConstraintToWorld tracked every one) are ours to destroy, and
		// leaving the vector populated left stale pointers that the next
		// world's removeConstraintFromWorld could match by address.
		qDeleteAll(constraints);
		constraints.clear();
		for (auto &handle : pickingHandles) {
			handle.activePickingConstraint = nullptr;
			handle.activeRigidBodyBeingManipulated = nullptr;
		}

		for (i = world->getNumCollisionObjects() - 1; i >= 0; i--) {
			btCollisionObject* obj = world->getCollisionObjectArray()[i];
			btRigidBody* body = btRigidBody::upcast(obj);
			if (body && body->getMotionState()) {
				delete body->getMotionState();
			}
			world->removeCollisionObject(obj);
			delete obj;
		}

		// https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=8148#p28087
		btOverlappingPairCache* pair_cache = world->getBroadphase()->getOverlappingPairCache();
		btBroadphasePairArray& pair_array = pair_cache->getOverlappingPairArray();
		for (int i = 0; i < pair_array.size(); i++)
			pair_cache->cleanOverlappingPair(pair_array[i], world->getDispatcher());
	}

	// Delete collision shapes. This loop used to iterate an ALWAYS-EMPTY array
	// (storeCollisionShape had no call sites), so every shape built for every
	// body leaked on each play/stop cycle — deep audit 2026-09, area 4 F3.
	// Order: front to back, so a btCompoundShape is destroyed before the child
	// shapes it points at.
	for (int j = 0; j < collisionShapes.size(); j++) {
		btCollisionShape* shape = collisionShapes[j];
		delete shape;
	}

	collisionShapes.clear();

	// AFTER the shapes: btConvexTriangleMeshShape reads its striding interface
	// on the way out.
	qDeleteAll(meshInterfaces);
	meshInterfaces.clear();

	delete world;
	world = 0;

	delete solver;
	solver = 0;

	delete broadphase;
	broadphase = 0;

	// After the broadphase — its pair cache holds the pointer.
	delete ghostPairCallback;
	ghostPairCallback = 0;

	delete dispatcher;
	dispatcher = 0;

	delete collisionConfig;
	collisionConfig = 0;

	hashBodies.clear();
	hashBodies.squeeze();
	// nodeTransforms is deliberately NOT cleared here: restartPhysics() runs
	// this, and its caller then calls restoreNodeTransformations() to put the
	// scene back where it was before Play. Clearing it would restore every
	// physics body to the identity transform.
}

}