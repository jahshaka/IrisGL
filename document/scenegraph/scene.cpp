/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "document/scenegraph/scene.h"
#include "document/scenegraph/scenenode.h"
#include "document/scenegraph/lightnode.h"
#include "document/scenegraph/decalnode.h"
#include "document/scenegraph/cameranode.h"
#include "document/scenegraph/meshnode.h"
#include "document/scenegraph/particlesystemnode.h"
#include "document/scenegraph/scenepicking.h"
#include "document/assets/mesh.h"
#include "core/geometry/trimesh.h"
#include "core/irisutils.h"

#include "document/physics/environment.h"
#include "core/math/intersectionhelper.h"
#include <cmath>
#include <algorithm>
#include <QSet>

#include <QtMultimedia/QMediaPlayer>
// #include <QtMultimedia/QMediaPlaylist>

namespace iris
{

static constexpr float kPi = 3.14159265358979f;

// The ENGINE's own defaults (SKY-GPU): these are Ogre AtmosphereNpr's preset
// values, which is what the sky is drawn with. The dials they replace described
// a CPU bake that no longer exists.
SkyRealistic SkyRealistic::defaults()
{
    SkyRealistic s;
    // Ogre's AtmosphereNpr preset defaults, with its linear skyColour
    // (0.334, 0.57, 1.0) expressed as the sRGB colour a user would pick to mean
    // it — the decode at the boundary turns it back into those three numbers.
    s.density   = 0.47f;
    s.diffusion = 2.0f;
    s.horizon   = 0.025f;
    s.skyColour = QColor(157, 198, 255);
    s.power     = 1.0f;
    return s;
}

Scene::Scene()
{
    mGraphScene = graph::stagingScene();
    rootNode = SceneNode::create();
    rootNode->setName("World");

    clearColor = QColor(0,0,0,0);
    renderSky = true;
    // THE DEFAULT SKY: 96 grey (owner pick 1, SKY_LIGHT_SPEC.md §9.1 option ii).
    // It was 72 while the scene's light came from a separate 96-grey "Ambient
    // Color"; with ambient BEING the sky (D14) the sky has to carry that level
    // itself — srgb(96)/255 decoded is 0.117 of radiance against the old flat
    // path's 0.120, so the level is preserved to 2.5% and the Sky Light's
    // default stays an honest 1.0.
    skyColor = QColor(96, 96, 96);

    fogColor = QColor(250, 250, 250);
    fogStart = 100;
    fogEnd = 180;
    fogEnabled = true;
    // 2/(100+180) = 0.0071: the exponential density that keeps a 100..180 linear
    // fog looking like itself (see fogDensityFromLinear).
    fogDensity = fogDensityFromLinear(fogStart, fogEnd);
    fogHeightDensity = 0.0f;      // height layer off until asked for
    fogAtmosphere = false;        // the authored colour, until a scene asks for the sky's
    fogHeightFalloff = 0.1f;
    fogHeightLevel = 0.0f;
    fogBreakMinBrightness = 0.25f;
    fogBreakFalloff = 0.1f;

    // SHADOWS ARE ON (SUN_AND_LIGHT_DEFAULTS_SPEC §2.5): this was the one
    // field the constructor never assigned — an uninitialised bool that every
    // shipped creation path happened to set afterwards, so it was latent
    // rather than live. It is the scene-wide master switch; a light's own
    // Shadow Type is the per-light one, and both default to on.
    shadowEnabled = true;

    // global illumination is opt-in: off by default, everywhere, always
    giMode = GiMode::OFF;
    giQuality = GiQuality::MEDIUM;
    giNumBounces = 1;
    giUpdateBudget = 1;         // one probe re-capture per frame (FIX WAVE B1)
    giPccGrid = iris::Vec3(3, 2, 3);
    giDdgi = -1;                // auto: no tier has been applied to this scene yet
    giDdgiIntensity = 1.0f;     // the calibrated default; see scene.h
    giDdgiAmbient = 1.0f;       // the ambient fix on; see scene.h
    // The Photon quality tier this scene comes back at when GI is switched on
    // (GI_UNIFIED_SPEC P2, owner decision D2 — new scenes are Epic). GI itself
    // stays OFF here: a bare document renders nothing until a tier is applied,
    // which is what the editor's new-scene path and the reader do.
    giTier = 3;

    // anti-aliasing is opt-in like GI: off (1 sample) by default
    antiAliasing = 1;

    // shadow-map resolution: 0 = Auto, i.e. derive the one global atlas base
    // from the largest per-light request (the historical behaviour)
    shadowResolution = 0;

    // shadow filter: -1 = Auto, i.e. the softest quality any shadow-casting
    // light asked for (the historical derivation)
    shadowFilterTier = -1;

    // shadow-map budget: 0 = Auto, i.e. take the World Mode tier's value
    shadowMapBudget = 0;
    particleTimeScale = 1.0f;

    // Post chain: everything off. A scene only gets effects when the user picks
    // a World Mode (or turns a row on); nothing changes under anyone's feet.
    hdrEnabled = false;
    // +0.6, not 0. The shader's exposure term is 1024*e^(exposure-2) divided by
    // the scene's average log-luminance (DownScale03_SumLumEnd_ps.glsl), and
    // Ogre's sample tuned its 0 against content whose radiance runs to ~20.
    // Every texture we can import is 8-bit, so ours runs to ~1: at 0 the filmic
    // curve plus its grade tail lands mid-grey at about 0.31, and a default
    // scene visibly darkens the moment HDR comes on. +0.6 is ln(1.8), the
    // amount that puts mid-grey back where it was; it is a starting point the
    // auto-exposure then works from, not a fixed offset.
    exposure = 0.6f;
    // The adaptation window, verbatim from what the engine hard-coded before
    // the document could say anything about it (PostFxDesc's own defaults), so
    // a scene written before this field existed grades identically.
    exposureMin = -2.5f;
    exposureMax = 2.5f;
    bloomEnabled = false;
    bloomThreshold = 5.0f;
    bloomKnee = 2.0f;   // the width the engine used to hard-code (A-6)
    ssaoEnabled = false;
    ssaoScale = 1.0f;
    ssaoPower = 1.5f;
    ssaoRadius = 2.0f;
    smaaPreset = -1;
    ssrMode = 0;
    refractionsMode = 0;
    // Distortion defaults to AUTO: the pass costs nothing until a scene holds a
    // distortion material, so "off" would only mean a user who authored one had
    // to find a switch to see it (POST_LOOKS_SPEC §5.3).
    distortionMode = 1;
    distortionStrength = 1.0f;
    // Planar reflections: -1/0/-1 = "follow the world mode" on all three. A
    // fresh scene in Custom mode therefore reflects nothing until a mode is
    // applied or the user marks a reflector — the feature is scene-capable by
    // default (owner's "maximum realness") but never costs a whole extra scene
    // render before there is something to reflect.
    planarReflectionBudget = -1;
    planarReflectionResolution = 0;
    planarReflectionShadows = -1;

    // World Mode: -1 = Custom. A new scene starts with the field values above
    // and no tier applied; picking a mode (World panel or world.mode) is what
    // writes a tier through. POST_CHAIN_SPEC §12 decision 8 proposed defaulting
    // new scenes to Epic — that would turn VCT GI, 4x MSAA and a 4096 shadow
    // atlas on for every scene and every test, so it is left to the owner.
    worldMode = -1;
    worldOverrides = QJsonObject();
    folders.clear();

    // selection outline: width in Preferences units (SceneMirror maps it to the
    // inverted-hull scale as 1 + width/150); colour stays invalid = "never set",
    // the mirror then falls back to the historical selection yellow. The
    // primary colour is invalid by the same rule and is derived from the
    // outline colour when nobody sets one.
    outlineWidth = 3;
    outlinePrimaryColor = QColor();

    // sky init
    skyType = SkyType::SINGLE_COLOR;

    skyRealistic = SkyRealistic::defaults();

    // AUTOMATIC sun: the lowest forwardShadingPriority directional
    // (SUN_AND_LIGHT_DEFAULTS Q1). The sky follows it; nothing steers it.
    sunLightGuid = QString();

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
	skyDataRealistic.insert("density", skyRealistic.density);
	skyDataRealistic.insert("diffusion", skyRealistic.diffusion);
	skyDataRealistic.insert("horizon", skyRealistic.horizon);
	skyDataRealistic.insert("power", skyRealistic.power);
	{
		QJsonObject skyCol;
		skyCol["r"] = skyRealistic.skyColour.red();
		skyCol["g"] = skyRealistic.skyColour.green();
		skyCol["b"] = skyRealistic.skyColour.blue();
		skyCol["a"] = skyRealistic.skyColour.alpha();
		skyDataRealistic.insert("skyColour", skyCol);
	}

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

    meshes.reserve(100);
    particleSystems.reserve(100);

    environment = QSharedPointer<Environment>(new Environment());
	gravity = environment->getWorldGravity();
    // The possession slot (AVATAR_LOCOMOTION_SPEC §8.4). Owned for the scene's
    // whole life like the Environment: `playMode` is what the file carries, and
    // WHICH avatar is being driven never is.
    possession = QSharedPointer<AvatarPossession>(new AvatarPossession(this));

	ambientMusicVolume = 50;
	// NOT `new QMediaPlayer()` — see ensureMediaPlayer(). Constructing one here
	// put the Qt multimedia backend on the startup path of every process that
	// ever makes a Scene, which is all of them.
	mediaPlayer = nullptr;
    // playList = new QMediaPlaylist();
    // playList->setPlaybackMode(QMediaPlaylist::Loop);
}

// Build the ambient-music player on first play, not in the constructor.
//
// The constructor used to do `mediaPlayer = new QMediaPlayer()` unconditionally.
// Every Scene::create() therefore loaded the Qt multimedia (ffmpeg) backend and
// enumerated audio devices — a pipewire connect + PulseAudio fallback on a Linux
// desktop — and the editor makes several Scenes during shell setup
// (EngineAssetScene, the preview scenes, the editor scene), so it happened on
// every launch and in every headless suite. Nothing reaches this player until
// a world with an ambientMusicPath is opened (scenereader.cpp) or the World
// panel selects one. STABILITY_PROGRAM_SPEC Lane 6a.
//
// Note the player is parentless and Scene has no destructor, so it leaks — it
// always did; deferring it means it now only leaks when it is actually used.
void Scene::ensureMediaPlayer()
{
	if (!mediaPlayer) mediaPlayer = new QMediaPlayer();
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

void Scene::setAmbientMusic(QString path)
{

	ambientMusicPath = path;
	
}

void Scene::stopPlayingAmbientMusic()
{
	if (mediaPlayer) mediaPlayer->stop();   // never played: nothing to stop
}

void Scene::startPlayingAmbientMusic()
{
	ensureMediaPlayer();
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

// ---------------------------------------------------------------------------
// THE SUN — one resolver, asked by everything (SUN_AND_LIGHT_DEFAULTS Q1/Q1e)
// ---------------------------------------------------------------------------
// What this replaced: three rules that could disagree in any scene with more
// than one directional light, with nothing reporting the disagreement — the
// sky link's depth-first walk (services/sunlink.cpp), the GI bounce light's
// lowest-nodeId scan (irisgl/mirror/scenemirror.cpp) and Ogre's own
// castShadows-then-light-id sort. Re-parenting a light silently moved the
// first; engine creation order decided the third.
//
// Ordering is by (forwardShadingPriority, nodeId) and both halves matter: the
// priority is the author's row, and the nodeId tie-break makes two lights that
// both read 0 resolve the same way on every reload (R6 in the spec — the tie
// case is the one the test has to assert, not the happy case).

QVector<LightNodePtr> Scene::directionalLights() const
{
    QVector<LightNodePtr> out;
    for (const auto &light : lights) {
        if (!light) continue;
        if (light->lightType != LightType::Directional) continue;
        out.append(light);
    }
    std::sort(out.begin(), out.end(), [](const LightNodePtr &a, const LightNodePtr &b) {
        if (a->forwardShadingPriority != b->forwardShadingPriority)
            return a->forwardShadingPriority < b->forwardShadingPriority;
        return a->nodeId < b->nodeId;
    });
    return out;
}

LightNodePtr Scene::sunLight() const
{
    // 1. An explicit pin wins outright — but only while it names a live
    //    DIRECTIONAL light. A pin left behind by a deleted light resolves to
    //    the automatic answer instead of to nothing (the panel then reads
    //    "chosen automatically" again, which is the truth).
    if (!sunLightGuid.isEmpty()) {
        auto node = nodes.value(sunLightGuid);
        if (node) {
            auto light = node.dynamicCast<LightNode>();
            if (light && light->lightType == LightType::Directional) return light;
        }
    }
    // 2. Lowest priority, ties by creation order. 3. None — a legal, shipped
    //    state (Mirror Room, Showroom 2), never a warning.
    const auto dirs = directionalLights();
    return dirs.isEmpty() ? LightNodePtr() : dirs.first();
}

QString Scene::sunReason() const
{
    if (!sunLightGuid.isEmpty()) {
        auto node = nodes.value(sunLightGuid);
        if (node) {
            auto light = node.dynamicCast<LightNode>();
            if (light && light->lightType == LightType::Directional)
                return QStringLiteral("pinned");
        }
    }
    return directionalLights().isEmpty() ? QStringLiteral("none")
                                         : QStringLiteral("priority");
}

QVector<LightNodePtr> Scene::secondaryDirectionals() const
{
    const auto sun = sunLight();
    QVector<LightNodePtr> out;
    for (const auto &light : directionalLights())
        if (light != sun) out.append(light);
    return out;
}

int Scene::nextForwardShadingPriority() const
{
    // The lowest number nobody is using: the first directional gets 0, a
    // second slots into 1, and deleting the sun frees 0 again for the next one.
    QSet<int> used;
    for (const auto &light : lights)
        if (light && light->lightType == LightType::Directional)
            used.insert(light->forwardShadingPriority);
    int p = 0;
    while (used.contains(p)) ++p;
    return p;
}

// ---------------------------------------------------------------------------
// THE SKY LIGHT — one resolver, asked by everything (SKY_LIGHT_SPEC.md §2)
// ---------------------------------------------------------------------------
// Deliberately the SAME shape as sunLight() above, for the same reason: three
// consumers each with their own idea of "which one is the skylight" is how the
// sun's three rules came to disagree with nobody being told. One rule, here:
// the FIRST VISIBLE Sky Light in creation order. A hidden Sky Light does not
// light (hiding one is how a user turns the skylight off without deleting it,
// and how the `sky.duplicate` issue clears), and NO Sky Light means no ambient
// at all — 27 zero coefficients, a black ambient term, as decided (D14).
//
// No pin: there is no `skyLightGuid` and there should not be one. The sun's pin
// exists because a directional light is also a normal light an author places
// for its shadow; a second Sky Light is never useful, so the answer to two of
// them is the scene issue, not a picker.

QVector<LightNodePtr> Scene::skyLights() const
{
    QVector<LightNodePtr> out;
    for (const auto &light : lights) {
        if (!light) continue;
        if (light->lightType != LightType::Sky) continue;
        out.append(light);
    }
    // `lights` is a QHash by guid — hash order is not an order. Sort, as
    // directionalLights() does, so every reload resolves the same way.
    std::sort(out.begin(), out.end(), [](const LightNodePtr &a, const LightNodePtr &b) {
        return a->nodeId < b->nodeId;
    });
    return out;
}

LightNodePtr Scene::skyLight() const
{
    for (const auto &light : skyLights())
        if (light->isVisibleInScene()) return light;
    return LightNodePtr();
}

QString Scene::skyLightReason() const
{
    const auto all = skyLights();
    if (all.isEmpty()) return QStringLiteral("none");
    for (const auto &light : all)
        if (light->isVisibleInScene()) return QStringLiteral("first");
    return QStringLiteral("allHidden");
}

void Scene::updateSceneAnimation(float time)
{
    animTime = time;
    // A torn-down scene (cleanup() ran, root dropped) can still receive this
    // from a viewport unwinding stale play state on a scene switch — the
    // second frame of the crash-1788594910 class.
    if (rootNode) rootNode->updateAnimation(time);
}

float Scene::advance(float dt)
{
    // The clock ticks whether or not anything in the document consumes it:
    // the return value feeds the renderer's particle and shader-time delta,
    // which run in the editor too.
    const int steps = clock.advance(dt);
    const float simDt = float(clock.frameSeconds());
    if (!rootNode || steps == 0) return simDt;
    const bool simulating = environment && environment->isSimulating();
    if (!playing && !simulating) return simDt;

    // THE POSE, at the clock's time — once, not per step: clips and property
    // tracks are evaluated at an ABSOLUTE time (the document keeps the clock,
    // the evaluators are stateless), so only the last evaluation of a frame
    // can matter. Before the physics steps, as PlayBack always ordered it.
    if (playing) {
        animTime = float(clock.time());
        rootNode->updateAnimation(animTime);
    }

    const float h = float(SimulationClock::kStepSeconds);
    for (int i = 0; i < steps; ++i) {
        // POSSESSION, first half (AVATAR_LOCOMOTION_SPEC §8.4): the possessed
        // avatar is the ONE consumer of the gameplay input state, and its
        // intent is camera-relative. This must run BEFORE the physics step,
        // because Environment::updateAvatarMovement — which stepSimulation
        // calls — is what spends the input. Every OTHER registered avatar
        // steps with whatever input it has, which for an unpossessed one is
        // zero (unpossess clears it), so it idles rather than freezing.
        if (playing && possession) possession->routeInput(h);
        environment->stepSimulation(h);
    }

	// Iterate over all rigid bodies and update the corresponding scenenode
	QHashIterator<QString, btRigidBody*> physicsBodies(environment->hashBodies);
	while (physicsBodies.hasNext()) {
		physicsBodies.next();
		// Match the bodies' hash to the scenenode's and override the mesh's transform if it's a known physics body
		auto rigidBodyWorldTransform = physicsBodies.value()->getWorldTransform();
		// Get the matching scenenode. NULL-CHECKED (deep-audit F3): a body
		// whose node was deleted mid-simulation (or a stale hash after a
		// scene switch) otherwise dereferences null here every frame.
		auto mesh = nodes.value(physicsBodies.key());
		if (!mesh || mesh->disablePhysicsTransform)
			continue;

		// Since the physics is detached from the engine rendering, this is VERY important to retain object scale
		// Set our scenenode to the simulated transform for the duration of the sim
		// ONE write, not two (MIRROR_SCALE lane): setGlobalPos and setGlobalRot
		// each resolved this body's parent and inverted it, so a falling crate
		// paid two parent resolutions and two inverses per step for one pose.
		const auto pos = rigidBodyWorldTransform.getOrigin();
		const auto rot = rigidBodyWorldTransform.getRotation();
		mesh->setGlobalPosRot(iris::Vec3(pos.x(), pos.y(), pos.z()),
		                      iris::Quat(rot.w(), rot.x(), rot.y(), rot.z()));
	}

    // POSSESSION, second half: the spring arm follows the pose the steps just
    // produced, so the camera never lags the character by a frame. It writes
    // `camera` — which the block below then updates and re-derives matrices
    // for, exactly as it does for any other camera move.
    if (playing && possession) possession->updateFollowCamera();

	refresh();
	return simDt;
}

void Scene::refresh()
{
	if (!rootNode) return;
	// Cameras aren't always a part of the scene hierarchy, so their matrices are updated here
	if (!!camera) {
		camera->update(0.0f);
		camera->updateCameraMatrices();
	}
	rootNode->update(0.0f);
}

void Scene::rayCast(const iris::Vec3& segStart,
                    const iris::Vec3& segEnd,
                    QList<PickingResult>& hitList,
					uint64_t pickingMask,
					bool allowUnpickable)
{
    for (const iris::MeshPick &p :
         iris::picking::raycastMeshes(this, segStart, segEnd, pickingMask, allowUnpickable)) {
        PickingResult pick;
        pick.hitNode = p.node;
        pick.hitPoint = p.hitPoint;
        pick.distanceFromStartSqrd = p.distanceSqrd;
        pick.triangleIndex = p.triangleIndex;
        hitList.append(pick);
    }
}

void Scene::addNode(SceneNodePtr node)
{
    if (node->hasScene()) {
        //qDebug() << "Node already has scene";
        //throw "Node already has scene";
    }

    if (node->sceneNodeType == SceneNodeType::Light) {
        auto light = node.staticCast<iris::LightNode>();
        lights.insert(light->getGUID(), light);
    }

    if (node->sceneNodeType == SceneNodeType::Decal) {
        auto decal = node.staticCast<iris::DecalNode>();
        decals.insert(decal->getGUID(), decal);
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

    // CAMERAS_SPEC §3: scene-graph cameras. Reachable only now that the
    // CameraNode constructor sets its own type — before that a camera added to
    // a scene arrived here as an Empty and was never registered.
    if (node->sceneNodeType == SceneNodeType::Camera) {
        cameras.insert(node->getGUID(), node.staticCast<iris::CameraNode>());
    }

	nodes.insert(node->getGUID(), node);

    // CAMERAS_SPEC §5: a node read from a file already carries its attachment,
    // so registration happens HERE and not only in attachToSocket — otherwise a
    // saved socketed camera would come back detached (and stationary) on open.
    if (node->isSocketAttached()) registerSocketAttachment(node);
}

void Scene::registerSocketAttachment(const SceneNodePtr &node)
{
    if (node.isNull() || !node->isSocketAttached()) return;
    QList<SceneNodePtr> &list = socketAttachments[node->socketOwnerGuid];
    if (!list.contains(node)) list.append(node);
}

void Scene::unregisterSocketAttachment(const SceneNodePtr &node)
{
    if (node.isNull()) return;
    // The node's own guid is the wrong key here (the registry is keyed by
    // OWNER so a rig's pose is read once), and the owner field may ALREADY have
    // been cleared by the caller — so sweep every bucket rather than trusting
    // it. Buckets are tiny; there is one per rig that carries attachments.
    for (auto it = socketAttachments.begin(); it != socketAttachments.end(); ) {
        it.value().removeAll(node);
        if (it.value().isEmpty()) it = socketAttachments.erase(it);
        else ++it;
    }
}

bool Scene::attachToSocket(const SceneNodePtr &node, const QString &ownerGuid,
                           const QString &socketName, QString *error)
{
    const auto refuse = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (node.isNull()) return refuse(QStringLiteral("no node"));
    if (node->getGUID() == ownerGuid)
        return refuse(QStringLiteral("a node cannot ride its own socket"));

    const SceneNodePtr owner = nodes.value(ownerGuid);
    if (owner.isNull())
        return refuse(QStringLiteral("no node with id '%1'").arg(ownerGuid));
    if (owner->getSceneNodeType() != SceneNodeType::Mesh)
        return refuse(QStringLiteral("'%1' is not a mesh, so it carries no sockets")
                          .arg(owner->getName()));
    auto meshOwner = owner.staticCast<iris::MeshNode>();
    if (!meshOwner->findSocket(socketName))
        return refuse(QStringLiteral("'%1' has no socket named '%2'")
                          .arg(owner->getName(), socketName));

    // A cycle would be a transform feedback loop: the owner's pose is read to
    // place the attached node, and the owner's own world transform is part of
    // that pose. Refuse an owner that lives INSIDE the attached node's subtree.
    for (SceneNodePtr walk = owner; !walk.isNull(); walk = walk->getParent()) {
        if (walk == node)
            return refuse(QStringLiteral("'%1' is inside '%2' — attaching it would make the "
                                         "socket drive its own owner")
                              .arg(owner->getName(), node->getName()));
    }

    unregisterSocketAttachment(node);
    node->setSocketAttachment(ownerGuid, socketName);
    registerSocketAttachment(node);
    // ON THE SOCKET, not "wherever it happened to be, offset by the socket"
    // (AVATAR_RIG_PERF_SPEC decision D4). A rider's local transform is now
    // RELATIVE TO THE SOCKET and it sticks — which is what lets a user nudge a
    // sword in a hand — so attaching has to start from the socket itself, or a
    // prop placed across the room would ride the hand from across the room.
    // (Before D4 nothing had to zero it: the resolver overwrote the rider's
    // WORLD transform every frame, so its local meant nothing while attached.)
    node->setLocalPos(Vec3(0, 0, 0));
    node->setLocalRot(Quat());
    node->setLocalScale(Vec3(1, 1, 1));
    return true;
}

bool Scene::detachFromSocket(const SceneNodePtr &node)
{
    if (node.isNull() || !node->isSocketAttached()) return false;
    unregisterSocketAttachment(node);
    node->setSocketAttachment(QString(), QString());
    return true;
}

namespace {

/// Removes `node` from a typed registry keyed by guid.
///
/// A1.5 (ENGINEERING_DEBT_SPEC addendum item 5). Every branch of removeNode
/// used to say `hash.remove(hash.key(ptr))` — a REVERSE lookup, a linear scan
/// over the hash's values, four of them per removed node, on a container
/// addNode fills keyed by exactly the guid we already hold. Worse, when the
/// node was not registered at all `QHash::key()` returns a default-constructed
/// QString and `remove("")` then evicted whatever unrelated entry happened to
/// be keyed with the empty string.
///
/// The guid lookup is O(1) and is the registered case. The reverse lookup
/// survives only as the fallback for a node whose guid was changed AFTER
/// addNode registered it (nothing in the tree does that today — every
/// setGUID call site runs before the node is added — but the old code
/// tolerated it, and this is what "no behaviour change" means here); the
/// empty-key eviction does not survive, because it was a bug.
template <typename Hash, typename Ptr>
void removeRegistered(Hash &hash, const QString &guid, const Ptr &node)
{
    if (hash.remove(guid) > 0) return;
    const QString stale = hash.key(node);
    if (!stale.isEmpty()) hash.remove(stale);
}

}  // namespace

void Scene::removeNode(SceneNodePtr node)
{
    const QString guid = node->getGUID();

    if (node->sceneNodeType == SceneNodeType::Light) {
        removeRegistered(lights, guid, node.staticCast<iris::LightNode>());
    }

    if (node->sceneNodeType == SceneNodeType::Decal) {
        removeRegistered(decals, guid, node.staticCast<iris::DecalNode>());
    }

    if (node->sceneNodeType == SceneNodeType::Mesh) {
        removeRegistered(meshes, guid, node.staticCast<iris::MeshNode>());
    }

    if (node->sceneNodeType == SceneNodeType::ParticleSystem) {
        removeRegistered(particleSystems, guid, node.staticCast<iris::ParticleSystemNode>());
    }

    if (node->sceneNodeType == SceneNodeType::Camera) {
        cameras.remove(guid);
        // Deleting the ACTIVE camera falls back to the free viewer rather than
        // leaving a guid that resolves to nothing. Undo re-adds the node with
        // the same guid, so re-pointing play at it is one setActiveCamera —
        // but a play that happens in between must not render through a dead
        // pointer's last transform.
        if (activeCameraGuid == guid) activeCameraGuid.clear();
    }

	nodes.remove(guid);
    // The node stops riding anything. Whatever was riding IT keeps its bucket:
    // the owner simply stops resolving (SocketResolver counts it as stale
    // and moves nothing), so an UNDO of the delete — which re-adds the same
    // guid — puts every rider back on its socket with no second verb.
    unregisterSocketAttachment(node);

    for (const auto &child : node->children()) {
        removeNode(child);
    }
}

void Scene::setCamera(CameraNodePtr cameraNode)
{
    camera = cameraNode;
}

bool Scene::setActiveCamera(const QString &guid)
{
    if (guid.isEmpty()) {          // back to the free viewer
        activeCameraGuid.clear();
        return true;
    }
    if (!cameras.contains(guid)) return false;
    activeCameraGuid = guid;
    return true;
}

CameraNodePtr Scene::getActiveCamera() const
{
    if (activeCameraGuid.isEmpty()) return CameraNodePtr();
    return cameras.value(activeCameraGuid);
}

// ---- play state + possession (AVATAR_LOCOMOTION_SPEC §8.4/§8.5) -----------
//
// setPlaying is EDGE-DETECTING. Everything possession does at a play boundary
// hangs off the transition and not off the caller, which is the whole of §8.3
// rule 1: `editor.stop()` has no early-out by design (a script must always be
// able to force a real stop), so a second stop must find nothing left to do
// rather than faulting or re-running a release. Gate P6.
namespace {
/// Pre-order over the document children (childAt skips the engine's own helper
/// nodes and can answer null — a walk must not assume the range is dense).
void clearSoftMobility(SceneNode *n)
{
    if (!n) return;
    n->_setSoftMovable(false);
    const int kids = n->childCount();
    for (int i = 0; i < kids; ++i) clearSoftMobility(n->childAt(i));
}
} // namespace

void Scene::setPlaying(bool playing)
{
    if (this->playing == playing) return;
    this->playing = playing;
    // SOFT PROMOTION IS PLAY-SCOPED (REALTIME_REFLECTIONS_SPEC §3.3.3, owner
    // decision O3). A node that started moving during play with nothing
    // predicting it was treated as movable for the rest of that play session,
    // leaving a "ghost" of its bounce light at its authored spot. Stop puts the
    // transforms back (PlayBack) and the classification with them, so the next
    // settle refresh rebuilds the room around the real world again. Cleared on
    // BOTH edges: a second play must not inherit the last one's promotions.
    if (rootNode) clearSoftMobility(rootNode.data());
    if (!possession) return;
    if (playing) possession->onPlayStarted();
    else possession->onPlayStopped();
}

void Scene::setPlayMode(ScenePlayMode mode)
{
    if (playMode == mode) return;
    playMode = mode;
    // Re-arm a RUNNING scene: switching a playing scene to third-person should
    // hand the keys to a character now, not at the next stop/start.
    if (playing && possession) possession->onPlayStarted();
}

ScenePtr Scene::create()
{
    ScenePtr scene(new Scene());
    scene->rootNode->setScene(scene);

    return scene;
}

void Scene::setGraphScene(graph::SceneHandle target)
{
    if (!target) target = graph::stagingScene();
    if (target == mGraphScene) return;
    if (!rootNode) { mGraphScene = target; return; }

    // The scene manager we are LEAVING may already be gone — an engine scene
    // destroyed while a document was still bound to it (the mirror is supposed
    // to unbind first). Say so; walking those handles is a read-after-destroy.
    if (mGraphScene && !graph::sceneAlive(mGraphScene)) {
        // The scene manager died under us — an engine scene destroyed while a
        // document was still bound to it (SceneMirror is supposed to unbind
        // first). Every handle in it is stale, so nothing may WALK the tree:
        // the registries are the only safe enumeration, and they cover every
        // node the tree reached (SceneNode::setScene registers them all).
        qWarning("iris::Scene: the engine scene this document was bound to has already been "
                 "destroyed — every node handle in it is stale and its transforms are lost. "
                 "SceneMirror must unbind (setSource(null)) before Engine::destroyScene().");
        // The owner's engine objects died with the manager; firing the hook
        // would walk dead handles. Drop it.
        mGraphEvacuationHook = nullptr;
        rootNode->_setGraphNode(nullptr);
        for (const auto &n : nodes) if (n) n->_setGraphNode(nullptr);
        for (const auto &w : mDetached) if (auto n = w.lock()) n->_setGraphNode(nullptr);
        mDetached.clear();
        mGraphScene = target;
        return;
    }

    // The graph's current OWNER releases its engine-side objects while the
    // nodes still exist (see _setGraphEvacuationHook). One-shot: whoever binds
    // next registers a fresh hook.
    if (mGraphEvacuationHook) {
        auto evacuate = std::move(mGraphEvacuationHook);
        mGraphEvacuationHook = nullptr;
        evacuate();
    }

    rootNode->_migrateGraph(target, nullptr);
    // ...and everything that left the tree but is still held somewhere (the
    // undo stack). Anything that has since been re-attached elsewhere, or has
    // died, is dropped here rather than followed.
    const graph::SceneHandle leaving = mGraphScene;
    for (auto it = mDetached.begin(); it != mDetached.end();) {
        SceneNodePtr n = it->lock();
        if (!n) { it = mDetached.erase(it); continue; }
        if (graph::sceneOf(n->graphNode()) != leaving) { it = mDetached.erase(it); continue; }
        n->_migrateGraph(target, nullptr);
        ++it;
    }
    mGraphScene = target;
    // A migration REBUILDS every Ogre node under the new manager, and rebuilt
    // nodes are born SCENE_DYNAMIC — the document's static hints survive on
    // the handles but the graph state is silently lost (found 2026-09-05:
    // every project load lost its whole static classification the moment the
    // viewport bound the scene, because the reader's applyStaticDefaults ran
    // against the staging manager). Re-assert from the hints, top-down; the
    // staging manager is skipped — nothing renders there, and the next real
    // bind re-asserts anyway.
    if (target != graph::stagingScene() && rootNode) rootNode->reapplyStaticHints();
}

void Scene::rememberDetached(const SceneNodePtr &node)
{
    if (node.isNull()) return;
    for (auto it = mDetached.begin(); it != mDetached.end();) {
        if (it->isNull()) it = mDetached.erase(it);
        else if (it->lock() == node) return;
        else ++it;
    }
    mDetached.append(node.toWeakRef());
}

void Scene::setOutlineWidth(int width)
{
    outlineWidth = width;
}

void Scene::setOutlineColor(QColor color)
{
    outlineColor = color;
}

void Scene::setOutlinePrimaryColor(QColor color)
{
    outlinePrimaryColor = color;
}

Scene::~Scene()
{
    cleanup();
}

void Scene::cleanup()
{
    // Everything here is a STRONG reference the scene owns. The tree hangs off
    // rootNode; `nodes` (and the per-type hashes beside it) is a second strong
    // reference to every node in it, registered by SceneNode::setScene. Both
    // halves have to go or the subtree outlives the scene — which is exactly
    // what happened before the deep audit of 2026-09: `nodes` and `decals` were
    // not cleared here at all, so even after MainWindow's close path called
    // cleanup() every node in the world was still reachable (and, with the old
    // strong `SceneNode::scene`, still pinned the scene itself).
    // THE CHANGE COLLECTOR dies with this object, so nothing may still name
    // it: a node the undo stack or a command still holds would otherwise carry
    // a dangling NodeDirtySet* and mark into freed memory on its next write.
    for (const auto &n : nodes) if (n) n->_setDirtySet(nullptr);
    if (rootNode) rootNode->_setDirtySet(nullptr);
    mDirtySet.clear();

    camera.clear();
    rootNode.clear();

    skyTexture.clear();

    lights.clear();
    decals.clear();
    meshes.clear();
    particleSystems.clear();
    cameras.clear();
    nodes.clear();
    socketAttachments.clear();   // a third strong reference to attached nodes
}

}
