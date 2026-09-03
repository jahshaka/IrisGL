/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include <QQuaternion>
#include "document/scenegraph/scene.h"
#include "document/scenegraph/scenenode.h"
#include "document/scenegraph/lightnode.h"
#include "document/scenegraph/cameranode.h"
#include "document/scenegraph/viewernode.h"
#include "document/scenegraph/meshnode.h"
#include "document/scenegraph/particlesystemnode.h"
#include "document/assets/mesh.h"
#include "core/geometry/trimesh.h"
#include "core/irisutils.h"

#include "document/physics/environment.h"
#include "core/math/intersectionhelper.h"

#include <QtMultimedia/QMediaPlayer>
// #include <QtMultimedia/QMediaPlaylist>

namespace iris
{

static constexpr float kPi = 3.14159265358979f;

// --- SkyRealistic sun angles ------------------------------------------------
// The analytic sky uses the sun vector twice: normalized (the direction every
// scattering term takes) and as `sunPosY / 450000` (the sunfade day/night
// term). Storing it at radius kSunRadius makes both exact, and makes azimuth /
// elevation a lossless view of the same three floats.
void SkyRealistic::setSunAngles(float azimuthDegrees, float elevationDegrees)
{
    const float deg2rad = kPi / 180.0f;
    const float az = azimuthDegrees * deg2rad;
    const float el = qBound(-90.0f, elevationDegrees, 90.0f) * deg2rad;
    const float cosEl = std::cos(el);
    sunPosX = kSunRadius * cosEl * std::sin(az);
    sunPosY = kSunRadius * std::sin(el);
    sunPosZ = kSunRadius * cosEl * std::cos(az);
}

float SkyRealistic::sunAzimuth() const
{
    if (qFuzzyIsNull(sunPosX) && qFuzzyIsNull(sunPosZ)) return 0.0f;
    float deg = std::atan2(sunPosX, sunPosZ) * 180.0f / kPi;
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

float SkyRealistic::sunElevation() const
{
    const float len = std::sqrt(sunPosX * sunPosX + sunPosY * sunPosY + sunPosZ * sunPosZ);
    if (len < 1e-6f) return 0.0f;
    return std::asin(qBound(-1.0f, sunPosY / len, 1.0f)) * 180.0f / kPi;
}

// The Preetham model's own working ranges, not the legacy panel's degenerate
// corner (VISUAL_PARITY_SPEC item 1): the old turbidity .32 sat well below the
// model's 1..20 band and the old sun vector (10, 7, 10) pinned `sunfade` to a
// constant. Mid-morning sun, clear air.
SkyRealistic SkyRealistic::defaults()
{
    SkyRealistic s;
    s.luminance = 1.0f;
    s.reileigh = 2.0f;
    s.mieCoefficient = 0.005f;
    s.mieDirectionalG = 0.8f;
    s.turbidity = 2.0f;
    s.setSunAngles(135.0f, 40.0f);
    return s;
}

Scene::Scene()
{
    rootNode = SceneNode::create();
    rootNode->setName("World");

    clearColor = QColor(0,0,0,0);
    renderSky = true;
    skyColor = QColor(72, 72, 72);

    fogColor = QColor(250, 250, 250);
    fogStart = 100;
    fogEnd = 180;
    fogEnabled = true;

    // global illumination is opt-in: off by default, everywhere, always
    giMode = GiMode::OFF;
    giQuality = GiQuality::MEDIUM;
    giBoundsMin = QVector3D();          // min == max -> automatic bounds
    giBoundsMax = QVector3D();
    giNumBounces = 1;
    giAutoRefresh = true;
    giPccGrid = QVector3D(3, 2, 3);

    // anti-aliasing is opt-in like GI: off (1 sample) by default
    antiAliasing = 1;

    // shadow-map resolution: 0 = Auto, i.e. derive the one global atlas base
    // from the largest per-light request (the historical behaviour)
    shadowResolution = 0;

    // shadow filter: -1 = Auto, i.e. the softest quality any shadow-casting
    // light asked for (the historical derivation)
    shadowFilterTier = -1;

    // Post chain: everything off. A scene only gets effects when the user picks
    // a World Mode (or turns a row on); nothing changes under anyone's feet.
    hdrEnabled = false;
    exposure = 0.0f;
    bloomEnabled = false;
    bloomThreshold = 5.0f;
    ssaoEnabled = false;
    ssaoScale = 1.0f;
    ssaoPower = 1.5f;
    ssaoRadius = 2.0f;
    smaaPreset = -1;
    ssrMode = 0;
    refractionsMode = 0;

    // World Mode: -1 = Custom. A new scene starts with the field values above
    // and no tier applied; picking a mode (World panel or world.mode) is what
    // writes a tier through. POST_CHAIN_SPEC §12 decision 8 proposed defaulting
    // new scenes to Epic — that would turn VCT GI, 4x MSAA and a 4096 shadow
    // atlas on for every scene and every test, so it is left to the owner.
    worldMode = -1;
    worldOverrides = QJsonObject();

    // selection outline: width in Preferences units (SceneMirror maps it to the
    // inverted-hull scale as 1 + width/150); colour stays invalid = "never set",
    // the mirror then falls back to the historical selection yellow
    outlineWidth = 3;

    // sky init
    skyType = SkyType::SINGLE_COLOR;

    skyRealistic = SkyRealistic::defaults();

    // 256x128 equirect bake; 512/1024 are the sharper (slower) choices
    skyBakeResolution = 256;

    // sky-driven ambient: on by default (owner decision, VISUAL_PARITY item 3b)
    ambientFromSky = true;

	gradientTop = QColor(255, 0, 0);
	gradientMid = QColor(0, 255, 0);
	gradientBot = QColor(0, 0, 255);
	gradientOffset = .5f;

	skyGuid = IrisUtils::generateGUID();

	QJsonObject colObj;
	colObj["r"] = skyColor.red();
	colObj["g"] = skyColor.green();
	colObj["b"] = skyColor.blue();
	colObj["a"] = skyColor.alpha();

	skyDataSingleColor = QJsonObject();
	skyDataSingleColor.insert("skyColor", colObj);

	skyDataRealistic = QJsonObject();
	skyDataRealistic.insert("luminance", skyRealistic.luminance);
	skyDataRealistic.insert("reileigh", skyRealistic.reileigh);
	skyDataRealistic.insert("mieCoefficient", skyRealistic.mieCoefficient);
	skyDataRealistic.insert("mieDirectionalG", skyRealistic.mieDirectionalG);
	skyDataRealistic.insert("turbidity", skyRealistic.turbidity);
	skyDataRealistic.insert("sunPosX", skyRealistic.sunPosX);
	skyDataRealistic.insert("sunPosY", skyRealistic.sunPosY);
	skyDataRealistic.insert("sunPosZ", skyRealistic.sunPosZ);

	QJsonObject colTop;
	QColor top(255, 146, 138);
	colTop["r"] = top.red();
	colTop["g"] = top.green();
	colTop["b"] = top.blue();
	colTop["a"] = top.alpha();

	QJsonObject colMid;
	QColor mid("white");
	colMid["r"] = mid.red();
	colMid["g"] = mid.green();
	colMid["b"] = mid.blue();
	colMid["a"] = mid.alpha();

	QJsonObject colBot;
	QColor bot(64, 128, 255);
	colBot["r"] = bot.red();
	colBot["g"] = bot.green();
	colBot["b"] = bot.blue();
	colBot["a"] = bot.alpha();

	skyDataGradient = QJsonObject();
	skyDataGradient.insert("gradientTop", colTop);
	skyDataGradient.insert("gradientMid", colMid);
	skyDataGradient.insert("gradientBot", colBot);
	skyDataGradient.insert("gradientOffset", .73f);

	skyData.insert("SingleColor", skyDataSingleColor);
	skyData.insert("Realistic", skyDataRealistic);
	skyData.insert("Gradient", skyDataGradient);
	skyData.insert("Equirectangular", QJsonObject());
	skyData.insert("Cubemap", QJsonObject());

    // end sky init

    ambientColor = QColor(96, 96, 96);

    meshes.reserve(100);
    particleSystems.reserve(100);

	time = 0;

    environment = QSharedPointer<Environment>(new Environment());
	gravity = environment->getWorldGravity();

	ambientMusicVolume = 50;
	mediaPlayer = new QMediaPlayer();
    // mediaPlayer->setVolume(ambientMusicVolume);
    // playList = new QMediaPlaylist();
    // playList->setPlaybackMode(QMediaPlaylist::Loop);
}

void Scene::setSkyTexture(Texture2DPtr tex)
{
    skyTexture = tex;
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
}

void Scene::setSkyColor(QColor color)
{
    this->skyColor = color;
}

void Scene::setAmbientColor(QColor color)
{
    this->ambientColor = color;
}

void Scene::setAmbientMusic(QString path)
{

	ambientMusicPath = path;
	
}

void Scene::stopPlayingAmbientMusic()
{
	mediaPlayer->stop();
}

void Scene::startPlayingAmbientMusic()
{
	mediaPlayer->stop();
	//mediaPlayer = new QMediaPlayer();
    // playList->removeMedia(0);
    // //playList = new QMediaPlaylist();
    // playList->addMedia(QUrl::fromLocalFile(ambientMusicPath));
    // mediaPlayer->setPlaylist(playList);
	mediaPlayer->play();
}

void Scene::setAmbientMusicVolume(float volume)
{
	ambientMusicVolume = volume;
    // mediaPlayer->setVolume(volume);
}

void Scene::updateSceneAnimation(float time)
{
    animTime = time;
    rootNode->updateAnimation(time);
}

void Scene::update(float dt)
{
	if (!rootNode)
		return;

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

		if (mesh->disablePhysicsTransform)
			continue;

		// Since the physics is detached from the engine rendering, this is VERY important to retain object scale
		//auto simulatedTransform = QMatrix4x4(matrix).transposed();
		//simulatedTransform.scale(mesh->getLocalScale());
		// Set our scenenode to the simulated transform for the duration of the sim
		//mesh->setGlobalTransform(simulatedTransform);
		auto pos = rigidBodyWorldTransform.getOrigin();
		mesh->setGlobalPos(QVector3D(pos.x(), pos.y(), pos.z()));
		auto rot = rigidBodyWorldTransform.getRotation();
		mesh->setGlobalRot(QQuaternion(rot.w(), rot.x(), rot.y(), rot.z()));
	}

	// Cameras aren't always a part of the scene hierarchy, so their matrices are updated here
	if (!!camera) {
		camera->update(dt);
		camera->updateCameraMatrices();
	}

	rootNode->update(dt);
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
        viewers.remove(viewer->getGUID());

        // Removing a viewer only changes the ACTIVE viewer when it WAS the
        // active one; then any remaining viewer takes over, otherwise there is
        // no active viewer left. (The old else-branch walked the iterator to
        // constEnd() and dereferenced it — past-the-end read on every removal
        // of a non-active viewer.)
        if (vrViewer == viewer) {
            if (viewers.isEmpty()) vrViewer.reset();
            else vrViewer = *viewers.constBegin();
        }
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

    skyTexture.clear();

    lights.clear();
    meshes.clear();
    particleSystems.clear();
    viewers.clear();
}

}
