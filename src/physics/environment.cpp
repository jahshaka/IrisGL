#include "environment.h"

#include "src/scenegraph/viewernode.h"

#include "bullet3/src/btBulletDynamicsCommon.h"
#include "BulletDynamics/Character/btKinematicCharacterController.h"
#include "BulletCollision/CollisionDispatch/btGhostObject.h"

#include "charactercontroller.h"

namespace iris
{

Environment::Environment(iris::RenderList *debugList)
{
	worldYGravity = 15.f;

    createPhysicsWorld();
 
    simulating = false;

    debugRenderList = debugList;
    lineMat = iris::LineColorMaterial::create();
    lineMat.staticCast<iris::LineColorMaterial>()->setDepthBias(10.f);

	//activePickingConstraint = 0;
	pickingHandles[(int)PickingHandleType::LeftHand] = PickingHandle();
	pickingHandles[(int)PickingHandleType::RightHand] = PickingHandle();
	pickingHandles[(int)PickingHandleType::MouseButton] = PickingHandle();
}

Environment::~Environment()
{
    destroyPhysicsWorld();
}

void Environment::setDirection(QVector2D dir)
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

void Environment::removeBodyFromWorld(btRigidBody *body)
{
    if (!hashBodies.contains(hashBodies.key(body))) return;

    world->removeRigidBody(body);
    hashBodies.remove(hashBodies.key(body));
    nodeTransforms.remove(hashBodies.key(body));
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
    collisionShapes.push_back(shape);
}

void Environment::addConstraintToWorld(btTypedConstraint *constraint, bool disableCollisions)
{
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
	startTransform.setOrigin(PhysicsHelper::btVector3FromQVector3D(node->getGlobalPosition()));

	auto controller = new CharacterController;
	controller->setSiblingGuid(node->getGUID());
	controller->getGhostObject()->setWorldTransform(startTransform);

	world->addCollisionObject(controller->getGhostObject(), btBroadphaseProxy::CharacterFilter, btBroadphaseProxy::StaticFilter | btBroadphaseProxy::DefaultFilter);
	world->addAction(controller->getKinematicController());

	characterControllers.insert(node->getGUID(), controller);

	activeCharacterController = controller;
}

void Environment::removeCharacterControllerFromWorld(const QString &guid)
{
	if (!characterControllers.contains(guid)) return;
	auto controller = characterControllers.value(guid);
	characterControllers.remove(guid);
	delete controller;
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
				auto body = PhysicsHelper::createPhysicsBody(child, child->physicsProperty);
				if (body) addBodyToWorld(body, child);
			}

			if (child.staticCast<iris::ViewerNode>()->isActiveCharacterController()) {
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

void Environment::drawDebugShapes()
{
	iris::LineMeshBuilder builder; // *must* go out of scope...
	debugDrawer->setPublicBuilder(&builder);

	world->debugDrawWorld();

	QMatrix4x4 transform;
	transform.setToIdentity();
	debugRenderList->submitMesh(builder.build(), lineMat, transform);
}

void Environment::setDebugDrawFlags(bool state)
{
	if (state) {
		debugDrawer->setDebugMode(
			GLDebugDrawer::DBG_DrawAabb |
			GLDebugDrawer::DBG_DrawWireframe |
			GLDebugDrawer::DBG_DrawConstraints |
			GLDebugDrawer::DBG_DrawContactPoints |
			GLDebugDrawer::DBG_DrawConstraintLimits |
			GLDebugDrawer::DBG_DrawFrames);
	}
	else {
		debugDrawer->setDebugMode(GLDebugDrawer::DBG_NoDebug);
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
	sweepBP->getOverlappingPairCache()->setInternalGhostPairCallback(new btGhostPairCallback());
	broadphase = sweepBP;

	collisionConfig = new btDefaultCollisionConfiguration();
	dispatcher = new btCollisionDispatcher(collisionConfig);
	solver = new btSequentialImpulseConstraintSolver();
	world = new btDiscreteDynamicsWorld(dispatcher, broadphase, solver, collisionConfig);

	hashBodies.reserve(512);
	nodeTransforms.reserve(512);

	world->setGravity(btVector3(0, -worldYGravity, 0));
	world->getDispatchInfo().m_allowedCcdPenetration = 0.0001f;

	// http://bulletphysics.org/mediawiki-1.5.8/index.php/Bullet_Debug_drawer
	debugDrawer = new GLDebugDrawer;
	world->setDebugDrawer(debugDrawer);
}

void Environment::createPickingConstraint(PickingHandleType handleType, const QString &pickedNodeGUID, const btVector3 &hitPoint, const QVector3D &segStart, const QVector3D &segEnd)
{
	PickingHandle& handle = pickingHandles[(int)handleType];

	// Fetch our rigid body from the list stored in the world by guid
	handle.activeRigidBodyBeingManipulated = hashBodies.value(pickedNodeGUID);
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

	btVector3 rayFromWorld = iris::PhysicsHelper::btVector3FromQVector3D(segStart);
	btVector3 rayToWorld = iris::PhysicsHelper::btVector3FromQVector3D(segEnd);

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

void Environment::updatePickingConstraint(PickingHandleType handleType, const QMatrix4x4 &handTransformation)
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
		handle.activeRigidBodyBeingManipulated->forceActivationState(handle.activeRigidBodySavedState);
		handle.activeRigidBodyBeingManipulated->activate();
		removeConstraintFromWorld(handle.activePickingConstraint);
		handle.activePickingConstraint = 0;
		handle.activeRigidBodyBeingManipulated = 0;
		delete handle.activePickingConstraint;
		delete handle.activeRigidBodyBeingManipulated;
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
	if (world) {
		int i;
		for (i = world->getNumConstraints() - 1; i >= 0; i--) {
			world->removeConstraint(world->getConstraint(i));
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

	// delete collision shapes
	for (int j = 0; j < collisionShapes.size(); j++) {
		btCollisionShape* shape = collisionShapes[j];
		delete shape;
	}

	collisionShapes.clear();

	delete world;
	world = 0;

	delete solver;
	solver = 0;

	delete broadphase;
	broadphase = 0;

	delete dispatcher;
	dispatcher = 0;

	delete collisionConfig;
	collisionConfig = 0;

	hashBodies.clear();
	hashBodies.squeeze();
}

}