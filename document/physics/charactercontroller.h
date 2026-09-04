#ifndef CHARACTERCONTROLLER_H
#define CHARACTERCONTROLLER_H

#include "core/math/mat4.h"
#include <QString>

#include "BulletDynamics/Character/btKinematicCharacterController.h"
#include "BulletDynamics/Dynamics/btRigidBody.h"
#include "BulletCollision/CollisionShapes/btCapsuleShape.h"
#include "BulletCollision/CollisionDispatch/btGhostObject.h"

class CharacterController
{
public:
	CharacterController();
	/// Frees the bullet objects createController() allocated. The caller must
	/// have taken them out of the world first
	/// (Environment::removeCharacterControllerFromWorld does).
	~CharacterController();

	void createController();
    void update();

	bool isActive();
	void setActive(bool state);

	btKinematicCharacterController *getKinematicController();
	btPairCachingGhostObject *getGhostObject();

	QString getSiblingGuid();
	void setSiblingGuid(const QString &guid);

	const iris::Mat4 getTransform();

private:
	btKinematicCharacterController *controller;
	btPairCachingGhostObject *ghostObject;
	class btConvexShape *shapeObject;
	//class btRigidBody *rigidBody;

	// Should probably be false by default
	bool active = true;
	QString siblingGuid;
};

#endif // CHARACTERCONTROLLER_H