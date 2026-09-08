#include "core/math/vec.h"
#include "document/physics/physicshelper.h"

#include "core/geometry/trimesh.h"
#include "document/physics/environment.h"

#include <QDebug>

namespace iris
{

// Converts the relevant parts of Jahshaka's trimesh structure to a Bullet triangle mesh
btTriangleMesh *PhysicsHelper::btTriangleMeshShapeFromMesh(iris::MeshPtr mesh)
{
    btTriangleMesh *triMesh = new btTriangleMesh;

    for (int i = 0; i < mesh->getTriMesh()->triangles.count(); ++i) {
        auto triangle = mesh->getTriMesh()->triangles[i];
        btVector3 btVertexA(triangle.a.x(), triangle.a.y(), triangle.a.z());
        btVector3 btVertexB(triangle.b.x(), triangle.b.y(), triangle.b.z());
        btVector3 btVertexC(triangle.c.x(), triangle.c.y(), triangle.c.z());
        triMesh->addTriangle(btVertexA, btVertexB, btVertexC);
    }

    return triMesh;
}

// Converts the relevant parts of Jahshaka's trimesh structure to a Bullet triangle mesh
btConvexHullShape *PhysicsHelper::btConvexHullShapeFromMesh(iris::MeshPtr mesh)
{
    btConvexHullShape *shape = new btConvexHullShape;

    // addPoint()'s default recalculateLocalAabb=true walks EVERY point already
    // added, so adding n points cost O(n^2) — on the UI thread, at Play, for
    // every convex-hull body in the scene (deep audit 2026-09, area 4 F4).
    // One recalc at the end is the same AABB in O(n).
    const auto &triangles = mesh->getTriMesh()->triangles;
    for (int i = 0; i < triangles.count(); ++i) {
        auto triangle = triangles[i];
        btVector3 btVertexA(triangle.a.x(), triangle.a.y(), triangle.a.z());
        btVector3 btVertexB(triangle.b.x(), triangle.b.y(), triangle.b.z());
        btVector3 btVertexC(triangle.c.x(), triangle.c.y(), triangle.c.z());
        shape->addPoint(btVertexA, false);
        shape->addPoint(btVertexB, false);
        shape->addPoint(btVertexC, false);
    }
    shape->recalcLocalAabb();

    return shape;
}

btVector3 PhysicsHelper::btVector3FromVec3(iris::Vec3 vector)
{
    return btVector3(vector.x(), vector.y(), vector.z());
}

iris::Vec3 PhysicsHelper::vec3FromBtVector3(btVector3 vector)
{
	return iris::Vec3(vector.getX(), vector.getY(), vector.getZ());
}

namespace {

/// The body PhysicsCollisionShape::None builds: a btEmptyShape at the node's
/// transform. It is also the DEGRADED result for a mesh-shaped body on a node
/// that carries no mesh geometry (see createPhysicsBody): the body still
/// exists — constraints, hashBodies and the transform sync keep resolving —
/// it simply collides with nothing, which is exactly what "no geometry" means.
void buildEmptyShapeBody(const btTransform &transform, btScalar mass,
                         btCollisionShape *&shape, btMotionState *&motionState,
                         btRigidBody *&body)
{
    shape = new btEmptyShape();
    motionState = new btDefaultMotionState(transform);
    btRigidBody::btRigidBodyConstructionInfo info(mass, motionState, shape);
    body = new btRigidBody(info);
    body->setCenterOfMassTransform(transform);
}

}  // namespace

// Every `new` below lands in `owned` — the body's shape, a compound's child
// shapes and the btTriangleMesh interfaces behind the mesh shapes. Bullet owns
// none of them (deep audit 2026-09, area 4 F3: they leaked, once per body, per
// Play). The caller destroys the set at world teardown.
PhysicsBody PhysicsHelper::createPhysicsBody(const iris::SceneNodePtr sceneNode, const iris::PhysicsProperty &props)
{
    PhysicsBody owned;
	iris::Vec3 globalPos = sceneNode->getGlobalPosition();
    btVector3 pos(globalPos.x(), globalPos.y(), globalPos.z());
    btRigidBody *body = nullptr;

    btTransform transform;
    transform.setIdentity();
	transform.setFromOpenGLMatrix(sceneNode->getGlobalTransform().constData());

    // A1.4 (ENGINEERING_DEBT_SPEC addendum item 4). This was an unguarded
    // `sceneNode.staticCast<iris::MeshNode>()` on ANY node carrying
    // `isPhysicsBody`. The UI only sets that flag on meshes — but the LOAD
    // path does not: SceneReader reads "physicsObject" for every node kind it
    // deserializes (scenereader.cpp), so a hand-authored or third-party scene
    // that flags a light, a camera or an empty as a physics body reached the
    // mesh-only branches below and read a MeshNode member off an object that
    // never had one.
    //
    // Nothing the shape-independent code needs is MeshNode's: transform,
    // rotation and scale are all SceneNode's, and they are read from
    // `sceneNode` now. Only the three geometry shapes need the real mesh, and
    // they degrade to an empty shape when it is not there.
    const auto meshNode = sceneNode.dynamicCast<iris::MeshNode>();
    const bool hasMeshGeometry = !meshNode.isNull() && !meshNode->getMesh().isNull()
                                 && meshNode->getMesh()->getTriMesh() != nullptr;
    auto rot = sceneNode->getGlobalRotation().toVector4D();

    btQuaternion quat;
    quat.setX(rot.x());
    quat.setY(rot.y());
    quat.setZ(rot.z());
    quat.setW(rot.w());

    btScalar mass = props.objectMass;
	btScalar bounciness = props.objectRestitution;
	btScalar margin = props.objectCollisionMargin;
	btScalar friction = props.objectFriction;

    btCollisionShape *shape = nullptr;
    btVector3 inertia(0, 0, 0);
    btMotionState *motionState = nullptr;

    switch (static_cast<int>(props.shape)) {
        case static_cast<int>(PhysicsCollisionShape::None): {
            transform.setFromOpenGLMatrix(sceneNode->getLocalTransform().constData());
            transform.setOrigin(pos);
            transform.setRotation(quat);

            buildEmptyShapeBody(transform, mass, shape, motionState, body);

            break;
        }

        case static_cast<int>(PhysicsCollisionShape::Sphere) : {

            transform.setRotation(quat);
            transform.setOrigin(pos);

            float rad = 1.0;

            shape = new btSphereShape(rad);
            shape->setLocalScaling(iris::PhysicsHelper::btVector3FromVec3(sceneNode->getLocalScale()));
            shape->setMargin(margin);
            motionState = new btDefaultMotionState(transform);

            btVector3 inertia(0, 0, 0);
            
            if (mass != 0.0) shape->calculateLocalInertia(mass, inertia);
            
            btRigidBody::btRigidBodyConstructionInfo info(mass, motionState, shape, inertia);
            body = new btRigidBody(info);
            body->setRestitution(bounciness);
			body->setFriction(friction);
            body->setCenterOfMassTransform(transform);

            break;
        }

        case static_cast<int>(PhysicsCollisionShape::Plane) : {

            transform.setOrigin(pos);
            transform.setRotation(quat);

            shape = new btStaticPlaneShape(btVector3(0, 1, 0), 0.f);
            shape->setLocalScaling(iris::PhysicsHelper::btVector3FromVec3(sceneNode->getLocalScale()));
            shape->setMargin(margin);
            motionState = new btDefaultMotionState(transform);

            if (mass != 0.0) shape->calculateLocalInertia(mass, inertia);

            btRigidBody::btRigidBodyConstructionInfo info(mass, motionState, shape);

            body = new btRigidBody(info);
            body->setRestitution(bounciness);
			body->setFriction(friction);
            body->setCenterOfMassTransform(transform);

            break;
        }

        case static_cast<int>(PhysicsCollisionShape::Cube) : {

            transform.setOrigin(pos);
            transform.setRotation(quat);

            shape = new btBoxShape(btVector3(1, 1, 1));
            shape->setLocalScaling(iris::PhysicsHelper::btVector3FromVec3(sceneNode->getLocalScale()));
            shape->setMargin(margin);
            motionState = new btDefaultMotionState(transform);

            if (mass != 0.0) shape->calculateLocalInertia(mass, inertia);

            btRigidBody::btRigidBodyConstructionInfo info(mass, motionState, shape, inertia);

            body = new btRigidBody(info);
            body->setRestitution(bounciness);
			body->setFriction(friction);
            body->setCenterOfMassTransform(transform);

            break;
        }

        // only show for mesh types!                                       
        case static_cast<int>(PhysicsCollisionShape::ConvexHull) : {
            transform.setOrigin(pos);
            transform.setRotation(quat);

            if (!hasMeshGeometry) {
                qWarning("PhysicsHelper: '%s' asks for a convex-hull body but carries no mesh "
                         "geometry — building an empty shape instead",
                         qUtf8Printable(sceneNode->getName()));
                buildEmptyShapeBody(transform, mass, shape, motionState, body);
                break;
            }

            // https://www.gamedev.net/forums/topic/691208-build-a-convex-hull-from-a-given-mesh-in-bullet/
            // https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=11342
            auto tmpShape = iris::PhysicsHelper::btConvexHullShapeFromMesh(meshNode->getMesh());
            tmpShape->setMargin(0); // bullet bug still?
            // tmpShape->setMargin(marginValue->getValue());

            // https://www.gamedev.net/forums/topic/602994-glmmodel-to-bullet-shape-btconvextrianglemeshshape/
            // alternatively instead of building a hull manually with points, use the triangle mesh
            // auto triMesh = iris::PhysicsHelper::btTriangleMeshShapeFromMesh(meshNode->getMesh());
            // btConvexShape *tmpshape = new btConvexTriangleMeshShape(triMesh);
            // btShapeHull *hull = new btShapeHull(tmpshape);
            // btScalar margin = tmpshape->getMargin();

            btShapeHull *hull = new btShapeHull(static_cast<btConvexHullShape*>(tmpShape));
            hull->buildHull(0);

            btConvexHullShape* pConvexHullShape = new btConvexHullShape(
                (const btScalar*) hull->getVertexPointer(), hull->numVertices(), sizeof(btVector3));
            shape = pConvexHullShape;
            delete hull;
            delete tmpShape;

            shape->setLocalScaling(iris::PhysicsHelper::btVector3FromVec3(sceneNode->getLocalScale()));

            motionState = new btDefaultMotionState(transform);

            if (mass != 0.0) shape->calculateLocalInertia(mass, inertia);

            btRigidBody::btRigidBodyConstructionInfo info(mass, motionState, shape, inertia);

            body = new btRigidBody(info);
            body->setRestitution(bounciness);
			body->setFriction(friction);
            body->setCenterOfMassTransform(transform);

            break;
        }

        case static_cast<int>(PhysicsCollisionShape::TriangleMesh) : {

            transform.setOrigin(pos);
            transform.setRotation(quat);

            if (!hasMeshGeometry) {
                qWarning("PhysicsHelper: '%s' asks for a triangle-mesh body but carries no mesh "
                         "geometry — building an empty shape instead",
                         qUtf8Printable(sceneNode->getName()));
                buildEmptyShapeBody(transform, mass, shape, motionState, body);
                break;
            }

            // convert triangle mesh into convex shape

            auto triMesh = iris::PhysicsHelper::btTriangleMeshShapeFromMesh(meshNode->getMesh());
            owned.meshInterfaces.append(triMesh);   // outlives the shape below

            shape = new btConvexTriangleMeshShape(triMesh, true);
            shape->setLocalScaling(iris::PhysicsHelper::btVector3FromVec3(sceneNode->getLocalScale()));
            shape->setMargin(margin);
            motionState = new btDefaultMotionState(transform);

            if (mass != 0.0) shape->calculateLocalInertia(mass, inertia);

            btRigidBody::btRigidBodyConstructionInfo info(mass, motionState, shape, inertia);

            body = new btRigidBody(info);
            body->setRestitution(bounciness);
			body->setFriction(friction);
            body->setCenterOfMassTransform(transform);

            break;
        }

		case static_cast<int>(PhysicsCollisionShape::Compound) : {

			auto rootTransformInverse = sceneNode->getGlobalTransform().inverted();

			std::function<void(btCollisionShape*, const SceneNodePtr)> createTriangleMeshAndAddToShape =
				[&](btCollisionShape *baseShape, const SceneNodePtr node)
			{
				// Same guard as the root's (A1.4): buildCompoundShape selects on the
				// node TYPE, and a node whose type says Mesh can still be a MeshNode
				// with no mesh loaded — or, from a hand-authored file, not a MeshNode
				// at all. A child that carries no geometry contributes nothing.
				auto childMeshNode = node.dynamicCast<iris::MeshNode>();
				if (childMeshNode.isNull() || childMeshNode->getMesh().isNull()
				    || !childMeshNode->getMesh()->getTriMesh()) {
					qWarning("PhysicsHelper: compound child '%s' carries no mesh geometry — skipped",
					         qUtf8Printable(node->getName()));
					return;
				}
				auto *childTriMesh = iris::PhysicsHelper::btTriangleMeshShapeFromMesh(childMeshNode->getMesh());
				owned.meshInterfaces.append(childTriMesh);
				auto childShape = new btConvexTriangleMeshShape(childTriMesh, true);
				owned.shapes.append(childShape);   // a compound does not own its children
				childShape->setMargin(margin);

				auto shapeTransform = rootTransformInverse * childMeshNode->getGlobalTransform();

				btTransform childTransform;
				childTransform.setIdentity();
				childTransform.setFromOpenGLMatrix(shapeTransform.constData());

				static_cast<btCompoundShape*>(baseShape)->addChildShape(childTransform, childShape);
			};

			std::function<void(btCollisionShape*, const SceneNodePtr)> buildCompoundShape =
				[&](btCollisionShape *rootShape, const SceneNodePtr node)
			{
				if (node->getSceneNodeType() == iris::SceneNodeType::Mesh) createTriangleMeshAndAddToShape(rootShape, node);
				
				for (auto child : node->children()) {
					if (child->getSceneNodeType() == iris::SceneNodeType::Mesh ||
						child->getSceneNodeType() == iris::SceneNodeType::Empty)
					{
						buildCompoundShape(rootShape, child);
					}
				}
			};

			shape = new btCompoundShape();
			buildCompoundShape(shape, sceneNode);
			shape->setMargin(margin);

			transform.setFromOpenGLMatrix(sceneNode->getGlobalTransform().constData());

			// Every child was skipped (no mesh geometry anywhere in the subtree):
			// an empty compound's AABB is the inverted initial one, so its inertia
			// tensor is meaningless. Degrade like the other geometry shapes.
			if (static_cast<btCompoundShape *>(shape)->getNumChildShapes() == 0) {
				delete shape;
				shape = nullptr;
				qWarning("PhysicsHelper: '%s' asks for a compound body but no node in its subtree "
				         "carries mesh geometry — building an empty shape instead",
				         qUtf8Printable(sceneNode->getName()));
				buildEmptyShapeBody(transform, mass, shape, motionState, body);
				break;
			}

			motionState = new btDefaultMotionState(transform);

			if (mass != 0.0) shape->calculateLocalInertia(mass, inertia);

			//btScalar masses[2] = { mass, mass / meshNode->children().count() };
			//static_cast<btCompoundShape*>(shape)->calculatePrincipalAxisTransform(masses, transform, inertia);

			btRigidBody::btRigidBodyConstructionInfo info(mass, motionState, shape, inertia);

			body = new btRigidBody(info);
			body->setRestitution(bounciness);
			body->setFriction(friction);
			body->setCenterOfMassTransform(transform);

			break;
		}

        default: break;
    }

    owned.body = body;
    if (shape) {
        // The body's own shape goes FIRST: destroyPhysicsWorld deletes the
        // array in order and a compound must die before its children.
        owned.shapes.prepend(shape);
    } else {
        // No body was built (unknown shape kind): nothing to hand back, and
        // nothing was allocated past this point.
        owned.shapes.clear();
        qDeleteAll(owned.meshInterfaces);
        owned.meshInterfaces.clear();
    }
    return owned;
}

btTypedConstraint * PhysicsHelper::createConstraintFromProperty(Environment *environment, const iris::ConstraintProperty & prop)
{
    btTypedConstraint *constraint = Q_NULLPTR;

    auto bodyA = environment->hashBodies.value(prop.constraintFrom);
    auto bodyB = environment->hashBodies.value(prop.constraintTo);
    // A saved constraint can name a node that no longer has a physics body
    // (deleted, or its isPhysicsBody flag cleared): hashBodies.value() then
    // returns null and the pivot reads below dereferenced it.
    if (!bodyA || !bodyB) return nullptr;

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

    if (prop.constraintType == iris::PhysicsConstraintType::Ball) {
        constraint = new btPoint2PointConstraint(
            *bodyA, *bodyB, frameA.getOrigin(), frameB.getOrigin()
        );
    }

    if (prop.constraintType == iris::PhysicsConstraintType::Dof6) {
        constraint = new btGeneric6DofConstraint(
            *bodyA, *bodyB, frameA, frameB, true
        );
    }

    return constraint;
}


}