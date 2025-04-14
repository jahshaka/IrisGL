#ifndef CHARACTERCONTROLLER_H
#define CHARACTERCONTROLLER_H

#include <QString>
#include <QMatrix4x4>

#include "BulletDynamics/Character/btKinematicCharacterController.h"
#include "BulletDynamics/Dynamics/btRigidBody.h"
#include "BulletCollision/CollisionShapes/btCapsuleShape.h"
#include "BulletCollision/CollisionDispatch/btGhostObject.h"

class CharacterController
{
public:
	CharacterController();

	void createController();
    void update();

	bool isActive();
	void setActive(bool state);

	btKinematicCharacterController *getKinematicController();
	btPairCachingGhostObject *getGhostObject();

	QString getSiblingGuid();
	void setSiblingGuid(const QString &guid);

	const QMatrix4x4 getTransform();

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