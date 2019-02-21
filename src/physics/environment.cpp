#include "environment.h"
#include <QDebug>

namespace iris
{

Environment::Environment(iris::RenderList *debugList)
{
    createPhysicsWorld();
 
    simulating = false;

    debugRenderList = debugList;
    lineMat = iris::LineColorMaterial::create();
    lineMat.staticCast<iris::LineColorMaterial>()->setDepthBias(10.f);

	activePickingConstraint = 0;
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
    iris::LineMeshBuilder builder; // *must* go out of scope...
    debugDrawer->setPublicBuilder(&builder);

    if (simulating) {
        world->stepSimulation(delta);
    }

    world->debugDrawWorld();

    QMatrix4x4 transform;
    transform.setToIdentity();
    debugRenderList->submitMesh(builder.build(), lineMat, transform);
}

void Environment::toggleDebugDrawFlags(bool state)
{
    if (!state) {
        debugDrawer->setDebugMode(GLDebugDrawer::DBG_NoDebug);
    }
    else {
        debugDrawer->setDebugMode(
            GLDebugDrawer::DBG_DrawAabb |
            GLDebugDrawer::DBG_DrawWireframe |
            GLDebugDrawer::DBG_DrawConstraints |
            GLDebugDrawer::DBG_DrawContactPoints |
            GLDebugDrawer::DBG_DrawConstraintLimits |
            GLDebugDrawer::DBG_DrawFrames
        );
    }
}

void Environment::startRigidBodyTeleport(const QString &guid)
{
	if (!hashBodies.contains(guid)) return;
	activeTeleportedRigidBody = hashBodies.value(guid);
}

void Environment::updateRigidBodyTeleport(const QString &guid, const QMatrix4x4 &transform)
{
	if (activeTeleportedRigidBody) {
		btTransform rigidBodyTransform;
		rigidBodyTransform.setIdentity();
		rigidBodyTransform.setFromOpenGLMatrix(transform.constData());

		activeTeleportedRigidBody->setWorldTransform(rigidBodyTransform);
		activeTeleportedRigidBody->getMotionState()->setWorldTransform(rigidBodyTransform);
		activeTeleportedRigidBody->setGravity(btVector3(0, 0, 0));
		activeTeleportedRigidBody->setLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
		activeTeleportedRigidBody->setAngularVelocity(btVector3(0.0f, 0.0f, 0.0f));
		activeTeleportedRigidBody->clearForces();
	}
}

void Environment::endRigidBodyTeleport()
{
	activeTeleportedRigidBody->setGravity(world->getGravity());
	activeTeleportedRigidBody = 0;
	delete activeTeleportedRigidBody;
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
    collisionConfig = new btDefaultCollisionConfiguration();
    dispatcher = new btCollisionDispatcher(collisionConfig);
    broadphase = new btDbvtBroadphase();
    solver = new btSequentialImpulseConstraintSolver();
    world = new btDiscreteDynamicsWorld(dispatcher, broadphase, solver, collisionConfig);

    hashBodies.reserve(512);
    nodeTransforms.reserve(512);

    world->setGravity(btVector3(0, -10.f, 0));

    // http://bulletphysics.org/mediawiki-1.5.8/index.php/Bullet_Debug_drawer
    debugDrawer = new GLDebugDrawer;
    debugDrawer->setDebugMode(GLDebugDrawer::DBG_NoDebug);
    world->setDebugDrawer(debugDrawer);
}

void Environment::createPickingConstraint(const QString &pickedNodeGUID, const btVector3 &hitPoint, const QVector3D &segStart, const QVector3D &segEnd)
{
	// Use a Point2Point Constraint, this makes the body follow the constraint origin loosely
	// with directional forces effecting its transform
	//if (pickedNode->isPhysicsBody) {
	//    activeRigidBody = scene->getPhysicsEnvironment()->hashBodies.value(pickedNode->getGUID());

	//    m_savedState = activeRigidBody->getActivationState();
	//    activeRigidBody->setActivationState(DISABLE_DEACTIVATION);
	//    //printf("pickPos=%f,%f,%f\n",pickPos.getX(),pickPos.getY(),pickPos.getZ());
	//    btVector3 localPivot = activeRigidBody->getCenterOfMassTransform().inverse() * iris::PhysicsHelper::btVector3FromQVector3D(hitList.last().hitPoint);
	//    btPoint2PointConstraint* p2p = new btPoint2PointConstraint(*activeRigidBody, localPivot);
	//    scene->getPhysicsEnvironment()->getWorld()->addConstraint(p2p, true);
	//    m_pickedConstraint = p2p;
	//    btScalar mousePickClamping = 30.f;
	//    p2p->m_setting.m_impulseClamp = mousePickClamping;
	//    //very weak constraint for picking
	//    p2p->m_setting.m_tau = 0.001f;
	//}

	//btVector3 rayFromWorld = iris::PhysicsHelper::btVector3FromQVector3D(editorCam->getGlobalPosition());
	//btVector3 rayToWorld = iris::PhysicsHelper::btVector3FromQVector3D(calculateMouseRay(point) * 1024);

	//m_oldPickingPos = rayToWorld;
	//m_hitPos = iris::PhysicsHelper::btVector3FromQVector3D(hitList.last().hitPoint);
	//m_oldPickingDist = (iris::PhysicsHelper::btVector3FromQVector3D(hitList.last().hitPoint) - rayFromWorld).length();

	// Fetch our rigid body from the list stored in the world by guid
	constraintActiveRigidBody = hashBodies.value(pickedNodeGUID);
	// prevent the picked object from falling asleep
	activeRigidBodySavedState = constraintActiveRigidBody->getActivationState();
	constraintActiveRigidBody->setActivationState(DISABLE_DEACTIVATION);
	// get the hit position relative to the body we hit 
	// constraints MUST be defined in local space coords
	btVector3 localPivot = constraintActiveRigidBody->getCenterOfMassTransform().inverse() * hitPoint;

	// create a transform for the pivot point
	btTransform pivot;
	pivot.setIdentity();
	pivot.setOrigin(localPivot);

	// create our constraint object
	auto dof6 = new btGeneric6DofConstraint(*constraintActiveRigidBody, pivot, true);
	bool bLimitAngularMotion = true;
	if (bLimitAngularMotion) {
		dof6->setAngularLowerLimit(btVector3(0, 0, 0));
		dof6->setAngularUpperLimit(btVector3(0, 0, 0));
	}

	// add the constraint to the world
	addConstraintToWorld(dof6, false);

	// store a pointer to our constraint
	activePickingConstraint = dof6;

	// define the 'strength' of our constraint (each axis)
	float cfm = 0.1f;
	dof6->setParam(BT_CONSTRAINT_STOP_CFM, cfm, 0);
	dof6->setParam(BT_CONSTRAINT_STOP_CFM, cfm, 1);
	dof6->setParam(BT_CONSTRAINT_STOP_CFM, cfm, 2);
	dof6->setParam(BT_CONSTRAINT_STOP_CFM, cfm, 3);
	dof6->setParam(BT_CONSTRAINT_STOP_CFM, cfm, 4);
	dof6->setParam(BT_CONSTRAINT_STOP_CFM, cfm, 5);

	// define the 'error reduction' of our constraint (each axis)
	float erp = 0.5f;
	dof6->setParam(BT_CONSTRAINT_STOP_ERP, erp, 0);
	dof6->setParam(BT_CONSTRAINT_STOP_ERP, erp, 1);
	dof6->setParam(BT_CONSTRAINT_STOP_ERP, erp, 2);
	dof6->setParam(BT_CONSTRAINT_STOP_ERP, erp, 3);
	dof6->setParam(BT_CONSTRAINT_STOP_ERP, erp, 4);
	dof6->setParam(BT_CONSTRAINT_STOP_ERP, erp, 5);

	// save this data for future reference
	//btVector3 rayFromWorld = iris::PhysicsHelper::btVector3FromQVector3D(editorCam->getGlobalPosition());
	//btVector3 rayToWorld = iris::PhysicsHelper::btVector3FromQVector3D(calculateMouseRay(point) * 1024);

	btVector3 rayFromWorld = iris::PhysicsHelper::btVector3FromQVector3D(segStart);
	btVector3 rayToWorld = iris::PhysicsHelper::btVector3FromQVector3D(segEnd);

	constraintOldPickingPosition = rayToWorld;
	constraintHitPosition = hitPoint;
	constraintOldPickingDistance = (hitPoint - rayFromWorld).length();
}

void Environment::updatePickingConstraint(const btVector3 &rayDirection, const btVector3 &cameraPosition)
{
	if (constraintActiveRigidBody && activePickingConstraint) {
		btGeneric6DofConstraint* pickingConstraint = static_cast<btGeneric6DofConstraint*>(activePickingConstraint);
		if (pickingConstraint) {
			// use another picking ray to get the target direction
			btVector3 dir = rayDirection;
			//-cameraPosition;
			dir.normalize();
			// use the same distance as when we originally picked the object
			dir *= constraintOldPickingDistance;
			btVector3 newPivot = cameraPosition + dir;
			// set the position of the constraint
			pickingConstraint->getFrameOffsetA().setOrigin(newPivot);
		}
	}
}

void Environment::cleanupPickingConstraint()
{
	if (activePickingConstraint) {
		constraintActiveRigidBody->forceActivationState(activeRigidBodySavedState);
		constraintActiveRigidBody->activate();
		removeConstraintFromWorld(activePickingConstraint);
		activePickingConstraint = 0;
		constraintActiveRigidBody = 0;
		delete activePickingConstraint;
		delete constraintActiveRigidBody;
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

    hashBodies.squeeze();
    nodeTransforms.squeeze();
}

}