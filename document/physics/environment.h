#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

// This class entails the simulated environment. That is, bullet utilized in iris.
// DO NOT use bullet specific types or functions in the main application, all objects should live here
// This includes any constraints, rigid bodies and bullet specific variables.
// If you need to use btVector3 etc somewhere, consider doing it here and deleting it after
// For example, all the rigid bodies in the scene are contained inside hashBodies

// See { bullet specific variables, bullet specific constraints }

#include "core/math/mat4.h"
#include "core/math/vec.h"
#include "document/physics/physicshelper.h"

#include <QVector>
#include <QHash>

#include "btBulletDynamicsCommon.h"

#include "document/physics/physicshelper.h"

class btTypedConstraint;
class btCollisionShape;
class btRigidBody;
class btCollisionConfiguration;
class btDispatcher;
class btBroadphaseInterface;
class btConstraintSolver;
class btDynamicsWorld;
class btStridingMeshInterface;
class btGhostPairCallback;

class CharacterController;

namespace iris
{

enum class PickingHandleType : int
{
	None,
	LeftHand,
	RightHand,
	MouseButton
};

struct PickingHandle
{
	btRigidBody *activeRigidBodyBeingManipulated = nullptr;
	btTypedConstraint *activePickingConstraint = nullptr;
	int	activeRigidBodySavedState;
	btVector3 constraintOldPickingPosition;
	btVector3 constraintHitPosition;
	btScalar constraintOldPickingDistance;
	PickingHandleType pickHandleType = PickingHandleType::None;
};

class Environment
{
public:

	bool walkForward = 0;
	bool walkBackward = 0;
	bool walkLeft = 0;
	bool walkRight = 0;
	iris::Vec2 walkDir;
	bool jump = 0;

    Environment();
    ~Environment();

	QHash<QString, CharacterController*> characterControllers;
	QHash<QString, btCollisionObject*> collisionObjects;
    QHash<QString, btRigidBody*> hashBodies;
    QHash<QString, iris::Mat4> nodeTransforms;

	void setDirection(iris::Vec2 dir);

	void addBodyToWorld(btRigidBody *body, const iris::SceneNodePtr &node);
	/// Adds the body AND takes ownership of every allocation behind it (the
	/// collision shape, a compound's children, the triangle-mesh interfaces).
	/// This is the only overload that leaves the world leak-free.
	void addBodyToWorld(PhysicsBody &owned, const iris::SceneNodePtr &node);
	void removeBodyFromWorld(btRigidBody *body);
	void removeBodyFromWorld(const QString &guid);

    void storeCollisionShape(btCollisionShape *shape);
    void storeMeshInterface(btStridingMeshInterface *iface);

    /// Collision shapes / mesh interfaces this world owns. Zero after
    /// destroyPhysicsWorld(); the play/stop gate asserts on both.
    int ownedShapeCount() const { return collisionShapes.size(); }
    int ownedMeshInterfaceCount() const { return meshInterfaces.size(); }

    void addConstraintToWorld(btTypedConstraint *constraint, bool disableCollisions = true);
    void removeConstraintFromWorld(btTypedConstraint *constraint);

	void addCharacterControllerToWorldUsingNode(const iris::SceneNodePtr &node);
	void removeCharacterControllerFromWorld(const QString &guid);
	/// Unregisters and destroys every character controller (teardown path).
	void removeAllCharacterControllersFromWorld();
	CharacterController *getActiveCharacterController();

	void initializePhysicsWorldFromScene(const iris::SceneNodePtr rootNode);
	void updateCharacterTransformFromSceneNode(const iris::SceneNodePtr rootNode);

    btDynamicsWorld *getWorld();

	void updateCharacterControllers(float delta);

    // These are special functions used for creating a constraint to drag bodies
	void simulatePhysics();
	bool isSimulating();
	void stopPhysics();
	void stopSimulation();
	void stepSimulation(float delta);

	void restoreNodeTransformations(iris::SceneNodePtr rootNode);

    void restartPhysics();
    void createPhysicsWorld();
    void destroyPhysicsWorld();

	// These manage a unique picking constraint that is used to manipulate a rigid body about a scene
	// Primarily used in the 3D viewport, the constraint can be loosened to behave more interactively
	void createPickingConstraint(PickingHandleType handleType, const QString &pickedNodeGUID, const btVector3 &hitPoint, const iris::Vec3 &segStart, const iris::Vec3 &segEnd);
	void updatePickingConstraint(PickingHandleType handleType, const btVector3 &rayDirection, const btVector3 &cameraPosition);
	void updatePickingConstraint(PickingHandleType handleType, const iris::Mat4 &handTransformation);
	void cleanupPickingConstraint(PickingHandleType handleType);

	void createConstraintBetweenNodes(iris::SceneNodePtr node, const QString &to, const iris::PhysicsConstraintType &type);
	void setWorldGravity(float gravity);
	float getWorldGravity();

private:
    btCollisionConfiguration    *collisionConfig;
    btDispatcher                *dispatcher;
    btBroadphaseInterface       *broadphase;
    btConstraintSolver          *solver;
    btDynamicsWorld             *world;
	
	QHash<int, PickingHandle> pickingHandles;

    QVector<btTypedConstraint*> constraints;
    /// Owned. Destroyed front-to-back at teardown, so a compound shape comes
    /// before its children (PhysicsHelper::createPhysicsBody builds it so).
    btAlignedObjectArray<btCollisionShape*>	collisionShapes;
    /// Owned, and destroyed AFTER collisionShapes: a btConvexTriangleMeshShape
    /// reads its striding interface in its own destructor path.
    QVector<btStridingMeshInterface*> meshInterfaces;
    /// The ghost-pair callback the broadphase's pair cache points at but does
    /// NOT own. One per world; deleted after the broadphase.
    btGhostPairCallback *ghostPairCallback = nullptr;

	btVector3 walkDirection;
	btScalar worldYGravity;

	CharacterController *activeCharacterController;

	/// Takes `controller` off the world's action list and out of the broadphase.
	void detachCharacterControllerFromWorld(CharacterController *controller);

    bool simulating;
    bool simulationStarted;

	/*
	btRigidBody *activeRigidBodyBeingManipulated;
	btTypedConstraint *activePickingConstraint;
	int	activeRigidBodySavedState;
	btVector3 constraintOldPickingPosition;
	btVector3 constraintHitPosition;
	btScalar constraintOldPickingDistance;
	*/
};

}

#endif // ENVIRONMENT_H