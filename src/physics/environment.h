#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

// This class entails the simulated environment. That is, bullet utilized in iris.
// DO NOT use bullet specific types or functions in the main application, all objects should live here
// This includes any constraints, rigid bodies and bullet specific variables.
// If you need to use btVector3 etc somewhere, consider doing it here and deleting it after
// For example, all the rigid bodies in the scene are contained inside hashBodies

// See { bullet specific variables, bullet specific constraints }

#include "physics/physicshelper.h"

#include <QVector>
#include <QHash>

#include "btBulletDynamicsCommon.h"
#include "LinearMath/btIDebugDraw.h"

#include "graphics/utils/linemeshbuilder.h"
#include "graphics/renderlist.h"
#include "graphics/renderitem.h"
#include "materials/linecolormaterial.h"

#include "physicshelper.h"

class btTypedConstraint;
class btCollisionShape;
class btRigidBody;
class btCollisionConfiguration;
class btDispatcher;
class btBroadphaseInterface;
class btConstraintSolver;
class btDynamicsWorld;

class CharacterController;

namespace iris
{

// Implement's bullets debug drawer to provide useful visual information about physics enabled entities
class GLDebugDrawer : public btIDebugDraw
{
    int m_debugMode;
    iris::RenderList *renderList;
    iris::LineMeshBuilder *builder;

public:
    GLDebugDrawer() {}
    virtual ~GLDebugDrawer() {}

    void setPublicBuilder(iris::LineMeshBuilder *builder) { this->builder = builder; }
	virtual void setDebugMode(int debugMode) { m_debugMode = debugMode; }
	virtual int getDebugMode() const { return m_debugMode; }

    virtual void drawLine(const btVector3& from, const btVector3& to, const btVector3& fromColor, const btVector3& toColor) {
        builder->addLine(
            QVector3D(from.x(), from.y(), from.z()),
            QColor(fromColor.getX() * 255.f, fromColor.getY() * 255.f, fromColor.getZ() * 255.f),
            QVector3D(to.x(), to.y(), to.z()),
            QColor(toColor.getX() * 255.f, toColor.getY() * 255.f, toColor.getZ() * 255.f)
        );
    }

    virtual void drawLine(const btVector3& from, const btVector3& to, const btVector3& color) {
        builder->addLine(
            QVector3D(from.x(), from.y(), from.z()),
            QColor(color.getX() * 255.f, color.getY() * 255.f, color.getZ() * 255.f),
            QVector3D(to.x(), to.y(), to.z()),
            QColor(color.getX() * 255.f, color.getY() * 255.f, color.getZ() * 255.f)
        );
    }

	// Implement these later if needed...
    virtual void drawSphere(const btVector3& p, btScalar radius, const btVector3& color) {}
    virtual void drawTriangle(const btVector3& a, const btVector3& b, const btVector3& c, const btVector3& color, btScalar alpha) {}
    virtual void drawContactPoint(const btVector3& PointOnB, const btVector3& normalOnB, btScalar distance, int lifeTime, const btVector3& color) {}
    virtual void reportErrorWarning(const char* warningString) {}
    virtual void draw3dText(const btVector3& location, const char* textString) {}
};

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
	QVector2D walkDir;
	bool jump = 0;

    Environment(iris::RenderList *renderList);
    ~Environment();

	QHash<QString, CharacterController*> characterControllers;
	QHash<QString, btCollisionObject*> collisionObjects;
    QHash<QString, btRigidBody*> hashBodies;
    QHash<QString, QMatrix4x4> nodeTransforms;

	void setDirection(QVector2D dir);

	void addBodyToWorld(btRigidBody *body, const iris::SceneNodePtr &node);
	void removeBodyFromWorld(btRigidBody *body);
	void removeBodyFromWorld(const QString &guid);

    void storeCollisionShape(btCollisionShape *shape);

    void addConstraintToWorld(btTypedConstraint *constraint, bool disableCollisions = true);
    void removeConstraintFromWorld(btTypedConstraint *constraint);

	void addCharacterControllerToWorldUsingNode(const iris::SceneNodePtr &node);
	void removeCharacterControllerFromWorld(const QString &guid);
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
	void drawDebugShapes();
    void setDebugDrawFlags(bool state);

	void restoreNodeTransformations(iris::SceneNodePtr rootNode);

    void restartPhysics();
    void createPhysicsWorld();
    void destroyPhysicsWorld();

	// These manage a unique picking constraint that is used to manipulate a rigid body about a scene
	// Primarily used in the 3D viewport, the constraint can be loosened to behave more interactively
	void createPickingConstraint(PickingHandleType handleType, const QString &pickedNodeGUID, const btVector3 &hitPoint, const QVector3D &segStart, const QVector3D &segEnd);
	void updatePickingConstraint(PickingHandleType handleType, const btVector3 &rayDirection, const btVector3 &cameraPosition);
	void updatePickingConstraint(PickingHandleType handleType, const QMatrix4x4 &handTransformation);
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
    btAlignedObjectArray<btCollisionShape*>	collisionShapes;

	btVector3 walkDirection;
	btScalar worldYGravity;

	CharacterController *activeCharacterController;

    bool simulating;
    bool simulationStarted;

    iris::MaterialPtr lineMat;
    iris::RenderList *debugRenderList;

    GLDebugDrawer *debugDrawer;
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