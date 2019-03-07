/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#include "scene.h"
#include "scenenode.h"
#include "../scenegraph/lightnode.h"
#include "../scenegraph/cameranode.h"
#include "../scenegraph/viewernode.h"
#include "../scenegraph/meshnode.h"
#include "../scenegraph/particlesystemnode.h"
#include "../scenegraph/grabnode.h"
#include "../graphics/mesh.h"
#include "../graphics/renderitem.h"
#include "../materials/defaultskymaterial.h"
#include "../geometry/trimesh.h"
#include "../core/irisutils.h"
#include "../graphics/renderlist.h"

#include "physics/environment.h"
#include "math/intersectionhelper.h"

namespace iris
{

Scene::Scene()
{
    rootNode = SceneNode::create();
    rootNode->setName("World");

    // todo: move this to ui code
    skyMesh = Mesh::loadMesh(":assets/models/sky.obj");

    clearColor = QColor(0,0,0,0);
    renderSky = true;
    skyMaterial = DefaultSkyMaterial::create();
    skyColor = QColor(255, 255, 255, 255);
    skyRenderItem = new RenderItem();
    skyRenderItem->mesh = skyMesh;
    skyRenderItem->material = skyMaterial;
    skyRenderItem->type = RenderItemType::Mesh;
    skyRenderItem->renderLayer = (int)RenderLayer::Background;

    fogColor = QColor(250, 250, 250);
    fogStart = 100;
    fogEnd = 180;
    fogEnabled = true;

    // temp
    skyType = SkyType::REALISTIC;

    skyRealistic.luminance = 1.0;
    skyRealistic.reileigh = 2.5;
    skyRealistic.mieCoefficient = 0.053;
    skyRealistic.mieDirectionalG = 0.75;
    skyRealistic.turbidity = .32f;
    skyRealistic.sunPosX = 10;
    skyRealistic.sunPosY = 7;
    skyRealistic.sunPosZ = 10;

    // end temp

    ambientColor = QColor(64, 64, 64);

    meshes.reserve(100);
    particleSystems.reserve(100);

    geometryRenderList = new RenderList();
    shadowRenderList = new RenderList();
    gizmoRenderList = new RenderList();

	time = 0;

    // switchSkyTexture(skyType);

    environment = QSharedPointer<Environment>(new Environment(geometryRenderList));
	gravity = environment->getWorldGravity();
}

void Scene::switchSkyTexture(iris::SkyType skyType)
{
    switch (static_cast<int>(skyType)) {
        case static_cast<int>(iris::SkyType::CUBEMAP): {
            skyMaterial->createProgramFromShaderSource(":assets/shaders/defaultsky.vert",
                                                       ":assets/shaders/cubemapsky.frag");
            break;
        }

        case static_cast<int>(iris::SkyType::EQUIRECTANGULAR): {
            skyMaterial->createProgramFromShaderSource(":assets/shaders/defaultsky.vert",
                                                       ":assets/shaders/equirectangularsky.frag");
            break;
        }

        case static_cast<int>(iris::SkyType::GRADIENT): {
            skyMaterial->createProgramFromShaderSource(":assets/shaders/defaultsky.vert",
                                                       ":assets/shaders/gradientsky.frag");
            break;
        }

        case static_cast<int>(iris::SkyType::MATERIAL): {
   /*         skyMaterial->createProgramFromShaderSource(":assets/shaders/defaultsky.vert",
                                                       ":assets/shaders/defaultsky.frag");*/
            break;
        }

        case static_cast<int>(iris::SkyType::REALISTIC): {
            skyMaterial->createProgramFromShaderSource(":assets/shaders/defaultsky.vert",
                                                       ":assets/shaders/realisticsky.frag");
            break;
        }

        case static_cast<int>(iris::SkyType::SINGLE_COLOR): {
            skyMaterial->createProgramFromShaderSource(":assets/shaders/defaultsky.vert",
                                                       ":assets/shaders/flatsky.frag");
        }
        default:
            break;
    }
}

void Scene::setSkyTexture(Texture2DPtr tex)
{
    skyTexture = tex;
    skyMaterial->setSkyTexture(tex);
}

void Scene::setWorldGravity(float gravity)
{
	environment->setWorldGravity(this->gravity = gravity);
}

QString Scene::getSkyTextureSource()
{
    return skyTexture->getSource();
}

void Scene::clearSkyTexture()
{
    skyTexture.clear();
    skyMaterial->clearSkyTexture();
}

void Scene::setSkyColor(QColor color)
{
    this->skyColor = color;
}

void Scene::setAmbientColor(QColor color)
{
    this->ambientColor = color;
}

void Scene::updateSceneAnimation(float time)
{
    rootNode->updateAnimation(time);
}

void Scene::update(float dt)
{
	time += dt < 0 ? 0 : dt;

    environment->stepSimulation(dt);

	// Iterate over all rigid bodies and update the corresponding scenenode
	QHashIterator<QString, btRigidBody*> physicsBodies(environment->hashBodies);
	while (physicsBodies.hasNext()) {
		physicsBodies.next();
		// Match the bodies' hash to the scenenode's and override the mesh's transform if it's a known physics body
		btScalar matrix[16];
		auto rigidBodyWorldTransform = physicsBodies.value()->getWorldTransform();
		// Put the transform matrix's float data into our array
		rigidBodyWorldTransform.getOpenGLMatrix(matrix);
		// Get the matching scenenode
		auto mesh = nodes.value(physicsBodies.key());
		// Since the physics is detached from the engine rendering, this is VERY important to retain object scale
		auto simulatedTransform = QMatrix4x4(matrix).transposed();
		simulatedTransform.scale(mesh->getLocalScale());
		// Set our scenenode to the simulated transform for the duration of the sim
		mesh->setGlobalTransform(simulatedTransform);
	}

	// Cameras aren't always a part of the scene hierarchy, so their matrices are updated here
	if (!!camera) {
		camera->update(dt);
		camera->updateCameraMatrices();
	}

	rootNode->update(dt);

	for (const auto &mesh : meshes) {
		mesh->submitRenderItems();
	}

    for (const auto &particle : particleSystems) {
        particle->submitRenderItems();
    }

    if (renderSky) this->geometryRenderList->add(skyRenderItem);
}

void Scene::render()
{

}

void Scene::rayCast(const QVector3D& segStart,
                    const QVector3D& segEnd,
                    QList<PickingResult>& hitList,
					uint64_t pickingMask,
					bool allowUnpickable)
{
    rayCast(rootNode, segStart, segEnd, hitList, pickingMask, allowUnpickable);
}

void Scene::rayCast(const QSharedPointer<iris::SceneNode>& sceneNode,
                    const QVector3D& segStart,
                    const QVector3D& segEnd,
                    QList<iris::PickingResult>& hitList,
					uint64_t pickingMask,
					bool allowUnpickable)
{
	if ((sceneNode->getSceneNodeType() == iris::SceneNodeType::Mesh) &&
		(sceneNode->isPickable() || allowUnpickable) &&
		(sceneNode->pickingGroups & pickingMask) == pickingMask)// check flag
	{
        auto meshNode = sceneNode.staticCast<iris::MeshNode>();
        auto mesh = meshNode->getMesh();
        if(mesh != nullptr)
        {
            
            // transform segment to local space
            auto invTransform = meshNode->globalTransform.inverted();
            auto a = invTransform * segStart;
            auto b = invTransform * segEnd;

			// ray-sphere intersection first
			auto mesh = meshNode->getMesh();
			auto sphere = mesh->getBoundingSphere();
			float t;
			QVector3D hitPoint;
			if (IntersectionHelper::raySphereIntersects(a, (b - a).normalized(), sphere.pos, sphere.radius, t, hitPoint)) {
				auto triMesh = meshNode->getMesh()->getTriMesh();

				QList<iris::TriangleIntersectionResult> results;
				if (int resultCount = triMesh->getSegmentIntersections(a, b, results)) {
					for (auto triResult : results) {
						// convert hit to world space
						auto hitPoint = meshNode->globalTransform * triResult.hitPoint;

						PickingResult pick;
						pick.hitNode = sceneNode;
						pick.hitPoint = hitPoint;
						pick.distanceFromStartSqrd = (hitPoint - segStart).lengthSquared();

						hitList.append(pick);
					}
				}
			}
        }
    }

    for (auto child : sceneNode->children) {
        rayCast(child, segStart, segEnd, hitList, pickingMask, allowUnpickable);
    }
}

void Scene::addNode(SceneNodePtr node)
{
    if (!!node->scene) {
        //qDebug() << "Node already has scene";
        //throw "Node already has scene";
    }

    if (node->sceneNodeType == SceneNodeType::Light) {
        auto light = node.staticCast<iris::LightNode>();
        lights.insert(light->getGUID(), light);
    }

    if (node->sceneNodeType == SceneNodeType::Mesh) {
		//qDebug() <<"Mesh GUID: " << node->getGUID();
        auto mesh = node.staticCast<iris::MeshNode>();
		if (meshes.contains(node->getGUID()))
			mesh->setGUID(IrisUtils::generateGUID());
		meshes.insert(node->getGUID(), mesh);
    }

    if (node->sceneNodeType == SceneNodeType::ParticleSystem) {
        auto particleSystem = node.staticCast<iris::ParticleSystemNode>();
        particleSystems.insert(node->getGUID(), particleSystem);
    }

    if (node->sceneNodeType == SceneNodeType::Viewer) {
        auto viewer = node.staticCast<iris::ViewerNode>();
        viewers.insert(node->getGUID(), viewer);

        if (!vrViewer) 
			vrViewer = viewer;
    }

	if (node->sceneNodeType == SceneNodeType::Grab) {
		auto grab = node.staticCast<iris::GrabNode>();
		grabbers.insert(node->getGUID(), grab);
	}

	nodes.insert(node->getGUID(), node);
}

void Scene::removeNode(SceneNodePtr node)
{
    if (node->sceneNodeType == SceneNodeType::Light) {
        lights.remove(lights.key(node.staticCast<iris::LightNode>()));
    }

    if (node->sceneNodeType == SceneNodeType::Mesh) {
        meshes.remove(meshes.key(node.staticCast<iris::MeshNode>()));
    }

    if (node->sceneNodeType == SceneNodeType::ParticleSystem) {
        particleSystems.remove(particleSystems.key(node.staticCast<iris::ParticleSystemNode>()));
    }

    if (node->sceneNodeType == SceneNodeType::Viewer) {
        auto viewer = node.staticCast<iris::ViewerNode>();
        viewers.remove(viewers.key(viewer));

        // Remove viewer and replace it if more viewers are available
        if (vrViewer == viewer && viewers.count() == 0) vrViewer.reset();
		else {
			auto iter = viewers.constBegin();
			while (iter != viewers.constEnd()) ++iter;
			vrViewer = iter.value();
		}
    }

	if (node->sceneNodeType == SceneNodeType::Grab) {
		auto grab = node.staticCast<iris::GrabNode>();
		grabbers.remove(grab->getGUID());
	}

	nodes.remove(node->getGUID());

    for (auto &child : node->children) {
        removeNode(child);
    }
}

void Scene::setCamera(CameraNodePtr cameraNode)
{
    camera = cameraNode;
}

ScenePtr Scene::create()
{
    ScenePtr scene(new Scene());
    scene->rootNode->setScene(scene);

    return scene;
}

void Scene::setOutlineWidth(int width)
{
    outlineWidth = width;
}

void Scene::setOutlineColor(QColor color)
{
    outlineColor = color;
}

void Scene::cleanup()
{
    camera.clear();
    rootNode.clear();
    vrViewer.clear();

    skyMesh.clear();
    skyTexture.clear();
    skyMaterial.clear();
    delete skyRenderItem;

    lights.clear();
    meshes.clear();
    particleSystems.clear();
    viewers.clear();

    delete geometryRenderList;
    delete shadowRenderList;
    delete gizmoRenderList;
}

}
