#include "charactercontroller.h"

CharacterController::CharacterController()
{
	createController();
}

void CharacterController::update()
{

}

bool CharacterController::isActive()
{
	return active;
}

void CharacterController::setActive(bool state)
{
	active = state;
}

btKinematicCharacterController *CharacterController::getKinematicController()
{
	return controller;
}

btPairCachingGhostObject *CharacterController::getGhostObject()
{
	return ghostObject;
}

QString CharacterController::getSiblingGuid()
{
	return siblingGuid;
}

void CharacterController::setSiblingGuid(const QString &guid)
{
	siblingGuid = guid;
}

const QMatrix4x4 CharacterController::getTransform()
{
	btScalar data[16];
	ghostObject->getWorldTransform().getOpenGLMatrix(data);
	return QMatrix4x4(data).transposed();
}

void CharacterController::createController()
{
	// Make these configurable in the future
	btScalar characterHeight = 5.75;
	btScalar characterWidth = 1.75;
	btScalar stepHeight = btScalar(1.f);

	shapeObject = new btCapsuleShape(characterWidth, characterHeight);

	ghostObject = new btPairCachingGhostObject();
	ghostObject->setCollisionShape(shapeObject);
	ghostObject->setCollisionFlags(btCollisionObject::CF_CHARACTER_OBJECT);

	controller = new btKinematicCharacterController(ghostObject, shapeObject, stepHeight);
	controller->setGravity(btVector3(0, -10, 0));
}