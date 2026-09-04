#include "core/math/mat4.h"
#include "core/math/vec.h"
#include "document/physics/environment.h"

#include "document/scenegraph/viewernode.h"

#include "btBulletDynamicsCommon.h"
#include "BulletDynamics/Character/btKinematicCharacterController.h"
#include "BulletCollision/CollisionDispatch/btGhostObject.h"

#include "document/physics/charactercontroller.h"

namespace iris
{

Environment::Environment()
{
	worldYGravity = 15.f;
	// Was left indeterminate: getActiveCharacterController() returned garbage
	// before the first character controller was ever created.
	activeCharacterController = nullptr;

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

void Environment::setDirection(iris::Vec2 dir)
{
	//walkDirection = btVector3(0.0, 0.0, 0.0);
	walkDir = dir;
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

void Environment::addCharacterControllerToWorldUsingNode(const iris::SceneNodePtr &node)
{
	btTransform startTransform;
	startTransform.setIdentity();
	startTransform.setOrigin(PhysicsHelper::btVector3FromVec3(node->getGlobalPosition()));

	auto controller = new CharacterController;
	controller->setSiblingGuid(node->getGUID());
	controller->getGhostObject()->setWorldTransform(startTransform);

	world->addCollisionObject(controller->getGhostObject(), btBroadphaseProxy::CharacterFilter, btBroadphaseProxy::StaticFilter | btBroadphaseProxy::DefaultFilter);
	world->addAction(controller->getKinematicController());

	characterControllers.insert(node->getGUID(), controller);

	activeCharacterController = controller;
}

// Exact mirror of addCharacterControllerToWorldUsingNode: the action comes off
// the world's action list and the ghost off the broadphase BEFORE the objects
// die. Deleting the controller while bullet still held those pointers left
// dangling entries that the next stepSimulation walked.
void Environment::removeCharacterControllerFromWorld(const QString &guid)
{
	if (!characterControllers.contains(guid)) return;
	auto controller = characterControllers.value(guid);
	characterControllers.remove(guid);

	detachCharacterControllerFromWorld(controller);

	if (activeCharacterController == controller) {
		activeCharacterController = characterControllers.isEmpty()
			? nullptr : *characterControllers.constBegin();
	}

	delete controller;
}

void Environment::detachCharacterControllerFromWorld(CharacterController *controller)
{
	if (!world || !controller) return;
	world->removeAction(controller->getKinematicController());
	world->removeCollisionObject(controller->getGhostObject());
}

void Environment::removeAllCharacterControllersFromWorld()
{
	for (auto controller : characterControllers) {
		detachCharacterControllerFromWorld(controller);
		delete controller;
	}
	characterControllers.clear();
	activeCharacterController = nullptr;
}

CharacterController *Environment::getActiveCharacterController()
{
	return activeCharacterController;
}

void Environment::initializePhysicsWorldFromScene(const iris::SceneNodePtr rootNode)
{
	std::function<void(const SceneNodePtr)> createPhysicsBodiesFromNode = [&](const SceneNodePtr node) {
		for (const auto child : node->children) {
			if (child->isPhysicsBody) {
				auto owned = PhysicsHelper::createPhysicsBody(child, child->physicsProperty);
				// The overload that ALSO takes the shapes: without it every
				// shape, compound child and triangle-mesh interface built here
				// leaked, once per Play.
				addBodyToWorld(owned, child);
			}

			// ONLY viewers carry the flag. The type test is load-bearing: the
			// unguarded staticCast used to read isActiveCharacterController out
			// of every node in the scene (a heap over-read past the end of a
			// MeshNode/LightNode, and a nonzero pad byte there spawned a bogus
			// character controller on a mesh or a light).
			if (child->sceneNodeType == SceneNodeType::Viewer &&
				child.staticCast<iris::ViewerNode>()->isActiveCharacterController()) {
				addCharacterControllerToWorldUsingNode(child);
			}

			createPhysicsBodiesFromNode(child);
		}
	};

	createPhysicsBodiesFromNode(rootNode);

	// now add constraints
	// TODO - avoid looping like this, get constraint list -- list and then use that
	// TODO - handle children of children?
	for (const auto &node : rootNode->children) {
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

void Environment::updateCharacterTransformFromSceneNode(const iris::SceneNodePtr node)
{
	btTransform ghostTransform;
	ghostTransform.setIdentity();
	ghostTransform.setFromOpenGLMatrix(node->getGlobalTransform().constData());
	if (!characterControllers.contains(node->getGUID())) return;
	characterControllers.value(node->getGUID())->getKinematicController()->getGhostObject()->setWorldTransform(ghostTransform);
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
		updateCharacterControllers(delta);
		//drawDebugShapes();
    }
}

void Environment::updateCharacterControllers(float delta)
{
	walkDirection = btVector3(0.0, 0.0, 0.0);
	btScalar walkVelocity = btScalar(1.1) * 5.0; // 4 km/h -> 1.1 m/s
	btScalar walkSpeed = walkVelocity * delta;

	for (auto controller : characterControllers) {
		if (controller->isActive()) {
			auto character = controller->getKinematicController();

			btTransform transform;
			transform = character->getGhostObject()->getWorldTransform();

			btVector3 forwardDir = transform.getBasis()[2];
			btVector3 upDir = transform.getBasis()[1];
			btVector3 strafeDir = transform.getBasis()[0];

			forwardDir.normalize();
			upDir.normalize();
			strafeDir.normalize();

			if (character->onGround() && jump) {
				character->jump(btVector3(0, 6, 0));
			}

			walkDirection += strafeDir * walkDir.x();
			walkDirection += forwardDir * walkDir.y();

			if (walkForward) {
				walkDirection -= forwardDir;
			}

			if (walkBackward) {
				walkDirection += forwardDir;
			}

			if (walkLeft) {
				walkDirection -= strafeDir;
			}

			if (walkRight) {
				walkDirection += strafeDir;
			}

			character->setWalkDirection(walkDirection * walkSpeed);

			break;
		}
	}
}

void Environment::restoreNodeTransformations(iris::SceneNodePtr rootNode)
{
	for (auto &node : rootNode->children) {
		if (node->isPhysicsBody) {
			node->setGlobalTransform(nodeTransforms.value(node->getGUID()));
		}
	}

	nodeTransforms.clear();
	nodeTransforms.squeeze();
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

	// Character controllers first: their ghost objects live in the collision
	// object array below, so they have to be unregistered and destroyed here or
	// the loop deletes the ghosts out from under the CharacterControllers the
	// hash still owns (the next play cycle then stepped dangling pointers).
	removeAllCharacterControllersFromWorld();

	if (world) {
		int i;
		for (i = world->getNumConstraints() - 1; i >= 0; i--) {
			world->removeConstraint(world->getConstraint(i));
		}

		// removeConstraint() only unregisters. The constraints WE created
		// (addConstraintToWorld tracked every one) are ours to destroy, and
		// leaving the vector populated left dangling pointers that the next
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