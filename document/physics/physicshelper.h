#ifndef PHYSICS_HELPER
#define PHYSICS_HELPER

#include "core/math/vec.h"
#include <QVector>

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

/// A rigid body plus EVERYTHING that was allocated to build it.
///
/// Bullet's collision shapes are not reference counted and a btRigidBody does
/// not own its shape; neither does a btCompoundShape own its children, nor a
/// mesh shape the btStridingMeshInterface behind it. Before this struct,
/// createPhysicsBody() returned the body alone and every one of those
/// allocations was dropped on the floor at each Play (`storeCollisionShape`
/// had zero call sites, so destroyPhysicsWorld's cleanup loop iterated an
/// always-empty array — deep audit 2026-09, area 4 F3). The Environment now
/// takes the whole set and destroys it in the right order at teardown.
struct PhysicsBody
{
    btRigidBody *body = nullptr;
    /// The body's collision shape first, then any compound children.
    QVector<btCollisionShape *> shapes;
    /// The triangle data behind btConvexTriangleMeshShape — must outlive the
    /// shape that reads it, so it is destroyed after `shapes`.
    QVector<btStridingMeshInterface *> meshInterfaces;
};

class PhysicsHelper
{
public:
    PhysicsHelper() = default;
    static btTriangleMesh *btTriangleMeshShapeFromMesh(iris::MeshPtr mesh);
    static btConvexHullShape *btConvexHullShapeFromMesh(iris::MeshPtr mesh);
    static btVector3 btVector3FromQVector3D(iris::Vec3 vector);
	static iris::Vec3 QVector3DFrombtVector3(btVector3 vector);
    /// Builds the body for `sceneNode`. The caller OWNS everything in the
    /// returned PhysicsBody (Environment::addBodyToWorld takes it over).
    /// `body` is null when the node's shape kind is not one we build.
    static PhysicsBody createPhysicsBody(const iris::SceneNodePtr sceneNode, const iris::PhysicsProperty &props);
    static btTypedConstraint *createConstraintFromProperty(Environment *environment, const iris::ConstraintProperty &prop);
};

}

#endif // PHYSICS_HELPER