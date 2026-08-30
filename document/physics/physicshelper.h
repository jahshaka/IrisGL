#ifndef PHYSICS_HELPER
#define PHYSICS_HELPER

#include <QVector3D>

#include "btBulletDynamicsCommon.h"
#include "BulletCollision/CollisionShapes/btConvexHullShape.h"
#include "BulletCollision/CollisionShapes/btShapeHull.h"
#include "BulletCollision/CollisionShapes/btConvexTriangleMeshShape.h"
#include "BulletCollision/CollisionShapes/btConvexHullShape.h"
#include "BulletCollision/CollisionShapes/btTriangleMesh.h"

#include "document/assets/mesh.h"
#include "document/physics/physicsproperties.h"
#include "document/scenegraph/scenenode.h"
#include "document/scenegraph/meshnode.h"
#include "irisglfwd.h"

namespace iris
{

class Environment;

class PhysicsHelper
{
public:
    PhysicsHelper() = default;
    static btTriangleMesh *btTriangleMeshShapeFromMesh(iris::MeshPtr mesh);
    static btConvexHullShape *btConvexHullShapeFromMesh(iris::MeshPtr mesh);
    static btVector3 btVector3FromQVector3D(QVector3D vector);
	static QVector3D QVector3DFrombtVector3(btVector3 vector);
    static btRigidBody *createPhysicsBody(const iris::SceneNodePtr sceneNode, const iris::PhysicsProperty &props);
    static btTypedConstraint *createConstraintFromProperty(Environment *environment, const iris::ConstraintProperty &prop);
};

}

#endif // PHYSICS_HELPER