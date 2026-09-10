#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

// This class entails the simulated environment. That is, bullet utilized in iris.
// DO NOT use bullet specific types or functions in the main application, all objects should live here
// This includes any constraints, rigid bodies and bullet specific variables.
// If you need to use btVector3 etc somewhere, consider doing it here and deleting it after
// For example, all the rigid bodies in the scene are contained inside hashBodies

// See { bullet specific variables, bullet specific constraints }

#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "document/physics/physicshelper.h"

#include <QVector>
#include <QHash>
#include <QSet>

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

    Environment();
    ~Environment();

	QHash<QString, btCollisionObject*> collisionObjects;
    QHash<QString, btRigidBody*> hashBodies;
    /// The pre-play LOCAL transform of every body and avatar, restored on
    /// Stop bit-exactly. It used to be the global MATRIX, put back through
    /// setGlobalTransform's decomposition — a lossy round trip that left a
    /// node a few ulps from where Play found it after every Simulate stop
    /// (the play path was saved by PlayBack's own exact restore running after
    /// it). Locals are what a parent-independent restore needs: a body is
    /// written in world space during the step and lands back in local space
    /// through the same parent it had.
    struct SavedLocal { iris::Vec3 pos, scale; iris::Quat rot; };
    QHash<QString, SavedLocal> nodeTransforms;

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

	void initializePhysicsWorldFromScene(const iris::SceneNodePtr rootNode);

    btDynamicsWorld *getWorld();

    // These are special functions used for creating a constraint to drag bodies
	void simulatePhysics();
	bool isSimulating();
	void stopPhysics();
	void stopSimulation();
	void stepSimulation(float delta);

	// ---- AVATARS (AVATAR_LOCOMOTION_SPEC §6.3) ---------------------------
	//
	// The seam the 2016 `updateCharacterControllers` call left behind, filled
	// with a component that owns NOTHING in the world. Every registered avatar
	// steps, possessed or not (§8.4: an unpossessed avatar must idle, not
	// freeze), and there is no `break` — the defect that made "which of two
	// characters walks" depend on QHash iteration order is not reproduced.
	void addAvatarToWorld(const iris::SceneNodePtr &node);
	void removeAvatarFromWorld(const QString &guid);
	void removeAllAvatarsFromWorld();
	int avatarCount() const { return avatars.size(); }
	/// Steps every registered avatar's movement component. Called from
	/// stepSimulation AFTER the rigid-body solve, so a sweep sees this frame's
	/// world.
	void updateAvatarMovement(float delta);

	/// Static triangle-mesh colliders for every `collisionEnabled` mesh that is
	/// not already a physics body (§6.3 option C). Built LAZILY — only when the
	/// scene actually contains an avatar — because the spec's own objection to
	/// option B was the seconds-per-Play cost of building a BVH for a whole
	/// level, and a scene with no character has nothing to collide with.
	void buildCollisionContent(const iris::SceneNodePtr &rootNode);

	void restoreNodeTransformations(iris::SceneNodePtr rootNode);
	void restoreNodeTransformationsRecursive(const iris::SceneNodePtr &node);

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

    /// The registered avatars, WEAKLY. A strong reference here would make the
    /// world the thing that keeps a deleted avatar alive, and "delete a moving
    /// avatar mid-play" (gate M7) would then be a leak rather than a removal.
    /// Expired entries are pruned by updateAvatarMovement.
    QVector<iris::SceneNodeWPtr> avatars;
    /// The guids of the nodes `buildCollisionContent` gave a static collider,
    /// so a second call is idempotent.
    QSet<QString> collisionContentNodes;

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

	btScalar worldYGravity;

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