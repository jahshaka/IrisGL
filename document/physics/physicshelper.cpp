#include "document/physics/physicshelper.h"

#include "core/geometry/trimesh.h"
#include "document/physics/environment.h"

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

btVector3 PhysicsHelper::btVector3FromQVector3D(QVector3D vector)
{
    return btVector3(vector.x(), vector.y(), vector.z());
}

QVector3D PhysicsHelper::QVector3DFrombtVector3(btVector3 vector)
{
	return QVector3D(vector.getX(), vector.getY(), vector.getZ());
}

// Every `new` below lands in `owned` — the body's shape, a compound's child
// shapes and the btTriangleMesh interfaces behind the mesh shapes. Bullet owns
// none of them (deep audit 2026-09, area 4 F3: they leaked, once per body, per
// Play). The caller destroys the set at world teardown.
PhysicsBody PhysicsHelper::createPhysicsBody(const iris::SceneNodePtr sceneNode, const iris::PhysicsProperty &props)
{
    PhysicsBody owned;
	QVector3D globalPos = sceneNode->getGlobalPosition();
    btVector3 pos(globalPos.x(), globalPos.y(), globalPos.z());
    btRigidBody *body = nullptr;

    btTransform transform;
    transform.setIdentity();
	transform.setFromOpenGLMatrix(sceneNode->getGlobalTransform().constData());

    auto meshNode = sceneNode.staticCast<iris::MeshNode>();
    auto rot = meshNode->getGlobalRotation().toVector4D();

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

            shape = new btEmptyShape();
            motionState = new btDefaultMotionState(transform);
            
            btRigidBody::btRigidBodyConstructionInfo info(mass, motionState, shape);
            body = new btRigidBody(info);
            body->setCenterOfMassTransform(transform);

            break;
        }

        case static_cast<int>(PhysicsCollisionShape::Sphere) : {

            transform.setRotation(quat);
            transform.setOrigin(pos);

            float rad = 1.0;

            shape = new btSphereShape(rad);
            shape->setLocalScaling(iris::PhysicsHelper::btVector3FromQVector3D(meshNode->getLocalScale()));
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
            shape->setLocalScaling(iris::PhysicsHelper::btVector3FromQVector3D(meshNode->getLocalScale()));
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
            shape->setLocalScaling(iris::PhysicsHelper::btVector3FromQVector3D(meshNode->getLocalScale()));
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

            shape->setLocalScaling(iris::PhysicsHelper::btVector3FromQVector3D(meshNode->getLocalScale()));

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

            // convert triangle mesh into convex shape

            auto triMesh = iris::PhysicsHelper::btTriangleMeshShapeFromMesh(meshNode->getMesh());
            owned.meshInterfaces.append(triMesh);   // outlives the shape below

            shape = new btConvexTriangleMeshShape(triMesh, true);
            shape->setLocalScaling(iris::PhysicsHelper::btVector3FromQVector3D(meshNode->getLocalScale()));
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

			auto rootTransformInverse = meshNode->getGlobalTransform().inverted();

			std::function<void(btCollisionShape*, const SceneNodePtr)> createTriangleMeshAndAddToShape =
				[&](btCollisionShape *baseShape, const SceneNodePtr node)
			{
				auto childMeshNode = node.staticCast<iris::MeshNode>();
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
				
				for (auto child : node->children) {
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

			motionState = new btDefaultMotionState(transform);

			if (mass != 0.0) shape->calculateLocalInertia(mass, inertia);

			//btScalar masses[2] = { mass, mass / meshNode->children.count() };
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