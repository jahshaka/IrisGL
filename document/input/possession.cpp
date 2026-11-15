/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/input/possession.h"

#include <QtGlobal>
#include <cmath>

#include "core/logger.h"
#include "core/math/mat4.h"
#include "document/input/inputmap.h"
#include "document/physics/avatarmovement.h"
#include "document/scenegraph/cameranode.h"
#include "document/scenegraph/meshnode.h"
#include "document/scenegraph/scene.h"
#include "document/scenegraph/scenenode.h"
#include "document/scenegraph/socket.h"

namespace iris
{

// --- the play mode's stable strings ----------------------------------------

const char *playModeName(ScenePlayMode mode)
{
    switch (mode) {
    case ScenePlayMode::ThirdPerson: return "third-person";
    case ScenePlayMode::Camera:      return "camera";
    case ScenePlayMode::Explorer:    break;
    }
    return "explorer";
}

bool playModeFromName(const QString &name, ScenePlayMode &out)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("explorer"))     { out = ScenePlayMode::Explorer;    return true; }
    if (n == QLatin1String("third-person")) { out = ScenePlayMode::ThirdPerson; return true; }
    if (n == QLatin1String("camera"))       { out = ScenePlayMode::Camera;      return true; }
    return false;
}

// --- helpers ---------------------------------------------------------------

namespace
{

/// The first skinned mesh in the subtree that carries `socketName`, depth-first.
/// A character's wrapper is the node the movement writes; the rig may be
/// several levels down (that is what avatar.spawn produces).
MeshNode *findSocketOwner(const SceneNodePtr &node, const QString &socketName)
{
    if (node.isNull()) return nullptr;
    if (node->getSceneNodeType() == SceneNodeType::Mesh) {
        auto *mesh = static_cast<MeshNode *>(node.data());
        if (mesh->hasSkeleton() && mesh->findSocket(socketName)) return mesh;
    }
    const int kids = node->childCount();
    for (int i = 0; i < kids; ++i) {
        SceneNode *raw = node->childAt(i);
        if (!raw) continue;
        if (auto *hit = findSocketOwner(raw->sharedFromThis(), socketName)) return hit;
    }
    return nullptr;
}

}   // namespace

// --- lifetime --------------------------------------------------------------

AvatarPossession::AvatarPossession(Scene *scene) : mScene(scene) {}
AvatarPossession::~AvatarPossession() = default;

// --- the slot --------------------------------------------------------------

SceneNodePtr AvatarPossession::possessed() const
{
    auto node = mPossessed.toStrongRef();
    // A possessed avatar whose component was removed (undo of avatar.spawn) is
    // no longer an avatar, and pretending otherwise would route input into a
    // null component every frame.
    if (node && !node->hasAvatarComponent()) return SceneNodePtr();
    return node;
}

QString AvatarPossession::possessedGuid() const
{
    return possessed().isNull() ? QString() : mPossessedGuid;
}

bool AvatarPossession::possess(const SceneNodePtr &node)
{
    if (node.isNull() || !node->hasAvatarComponent()) return false;

    const SceneNodePtr current = possessed();
    if (current && current.data() == node.data()) return true;   // already driving it

    // Unreal's rule (§8.4): Possess() on a controller that is already
    // controlling unpossesses first. It is also the only way to guarantee the
    // released avatar's input is ZEROED rather than frozen at its last value —
    // a held W at the moment of a switch would otherwise walk it forever.
    unpossess();

    mPossessed = node.toWeakRef();
    mPossessedGuid = node->getGUID();
    captureOffsetReference(node);

    // Start the arm behind the character rather than wherever the previous
    // possession left it: the camera's yaw is the avatar's heading.
    float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
    node->getGlobalRotation().getEulerAngles(&pitch, &yaw, &roll);
    mYaw = yaw;
    mPitch = 12.0f;

    updateFollowCamera();
    return true;
}

bool AvatarPossession::unpossess()
{
    const SceneNodePtr node = possessed();
    mPossessed.clear();
    mPossessedGuid.clear();
    if (node.isNull()) return false;
    // THE §8.4 REQUIREMENT, and gate P5: the released avatar keeps stepping
    // (its state machine must idle, not freeze) but with ZERO input.
    if (auto *movement = node->avatar()) movement->clearInput();
    return true;
}

// --- the follow camera -----------------------------------------------------

void AvatarPossession::setFollow(const FollowCameraParams &p)
{
    mFollow = p;
    mFollow.armLength = qMax(0.0f, mFollow.armLength);
    if (mFollow.maxPitch < mFollow.minPitch) qSwap(mFollow.minPitch, mFollow.maxPitch);
    setPitch(mPitch);
}

void AvatarPossession::setYaw(float degrees)
{
    // Wrapped, not clamped: a third-person camera turns all the way round.
    mYaw = std::fmod(degrees, 360.0f);
    if (mYaw > 180.0f) mYaw -= 360.0f;
    if (mYaw < -180.0f) mYaw += 360.0f;
}

void AvatarPossession::setPitch(float degrees)
{
    mPitch = qBound(mFollow.minPitch, degrees, mFollow.maxPitch);
}

Vec3 AvatarPossession::moveToWorld(float x, float y) const
{
    // The stick's axes, in the movement component's convention: +x is right,
    // +y is FORWARD, and forward is world -Z (avatarmovement.h states it once).
    // Rotating by the camera's YAW ONLY — never its pitch — is what makes
    // "forward" mean "away from the camera" and keeps a look-down from walking
    // the character into the floor.
    const Vec3 local(x, 0.0f, -y);
    if (local.x() == 0.0f && local.z() == 0.0f) return Vec3(0, 0, 0);
    return Quat::fromEulerAngles(0.0f, mYaw, 0.0f).rotatedVector(local);
}

void AvatarPossession::captureOffsetReference(const SceneNodePtr &node)
{
    // Defaults, in case the character has neither a socket nor a capsule.
    FollowCameraParams p = mFollow;

    if (auto *movement = node->avatar()) {
        // A capsule-height pivot is already better than a constant: a 1.8 u
        // human and a 0.6 u critter want different arms.
        p.heightOffset = qMax(0.2f, movement->params().capsuleHeight * 0.9f);
        p.armLength = qMax(1.0f, movement->params().capsuleHeight * 2.2f);
        p.lateralOffset = 0.0f;
    }

    // THE OFFSET REFERENCE, READ ONCE (§8.5). A bind-pose read: no engine, no
    // rendered frame, no per-frame bone ride — socketWorldTransform falls back
    // to bindPoseWorldTransforms when no pose source is installed, and that is
    // exactly the pose we want (the character's proportions, not its gait).
    if (MeshNode *owner = findSocketOwner(node, QStringLiteral("shoulder"))) {
        Mat4 socketWorld;
        if (socketWorldTransform(owner, QStringLiteral("shoulder"), BonePoseSource(), socketWorld)) {
            const Vec4 t = socketWorld.column(3);
            const Vec3 origin = node->getGlobalPosition();
            const float dy = t.y() - origin.y();
            const float dx = t.x() - origin.x();
            const float dz = t.z() - origin.z();
            if (dy > 0.05f) p.heightOffset = dy;
            // The horizontal distance from the character's centre line IS the
            // shoulder's lateral offset — which side it is on depends on the
            // rig's axes, and a third-person camera looks over the same
            // shoulder either way, so the magnitude is the honest reading.
            p.lateralOffset = std::sqrt(dx * dx + dz * dz);
        }
    }
    setFollow(p);
}

void AvatarPossession::updateFollowCamera()
{
    const SceneNodePtr node = possessed();
    if (node.isNull()) return;

    const Quat rot = Quat::fromEulerAngles(mPitch, mYaw, 0.0f);
    const Vec3 forward = rot.rotatedVector(Vec3(0, 0, -1));
    const Vec3 right = rot.rotatedVector(Vec3(1, 0, 0));

    // The pivot is on the WRAPPER (the node the movement component writes), not
    // on a bone: the wrapper does not bob, and that is the whole point of §8.5.
    const Vec3 pivot = node->getGlobalPosition()
                     + Vec3(0, mFollow.heightOffset, 0)
                     + right * mFollow.lateralOffset;

    mCameraPos = pivot - forward * mFollow.armLength;
    mCameraRot = rot;

    if (!mScene) return;
    const CameraNodePtr cam = mScene->camera;
    if (cam.isNull()) return;
    // R5: PlayBack::update calls setController(mouseController) every frame and
    // the mouse controller writes the camera in update(dt) — which runs BEFORE
    // Scene::update, i.e. before this. Writing here means the arm wins without
    // a second driver being installed beside the controller path, and
    // Scene::update's own camera->update(dt)/updateCameraMatrices runs after us.
    if (!mHasSavedCamera) {
        mSavedCameraPos = cam->getLocalPos();
        mSavedCameraRot = cam->getLocalRot();
        mHasSavedCamera = true;
    }
    cam->setLocalPos(mCameraPos);
    cam->setLocalRot(mCameraRot);
}

// --- the per-frame route ---------------------------------------------------

void AvatarPossession::routeInput(float dt)
{
    Q_UNUSED(dt);
    const SceneNodePtr node = possessed();
    if (node.isNull()) return;
    auto *movement = node->avatar();
    if (!movement) return;

    InputSystem &sys = InputSystem::instance();

    // Look drives the ARM, and draining the accumulator is a consumer's job.
    const InputAxis2D look = sys.consumeLook();
    if (look.x != 0.0f || look.y != 0.0f) {
        setYaw(mYaw - look.x * mFollow.yawSensitivity);
        setPitch(mPitch + look.y * mFollow.pitchSensitivity);
    }

    // Move is camera-relative (gate P2) — the ONE routing decision that is not
    // "which avatar".
    const InputState &state = sys.state();
    movement->setMoveInput(moveToWorld(state.move.x, state.move.y));
    movement->setSprint(state.sprint);
    // TRANSFER the latch. consumeJump() is the reader-and-clearer; the
    // component's own step is what finally spends it (§5). possess/unpossess
    // never call this — a verb that swallowed a pending jump would make the
    // latch depend on when a script happened to run.
    if (sys.consumeJump()) movement->requestJump();
}

// THE HOST HALF (see the header). `Scene::camera` and the camera a viewport
// actually renders through are NOT always the same node — measured, not
// assumed: on a scripted `project.create` the editor viewport rendered through
// `EngineSceneViewport::mEditorCam` while `Scene::camera` pointed at a
// different CameraNode, so a document-side write to `Scene::camera` moved
// nothing on screen (and `PlayerMouseController`, which drives the same
// `Scene::camera`, has the same exposure — reported upward, not fixed here).
// So the host, which is the only thing that knows which camera it renders,
// hands that camera in.
void AvatarPossession::applyToViewCamera(const CameraNodePtr &cam)
{
    if (cam.isNull()) return;
    if (!isPossessing()) {
        // Possession ended (stop, or an explicit unpossess mid-play): give the
        // host's camera back exactly where the arm found it. Idempotent — the
        // flag is what stops a second call restoring a stale pose over a pose
        // the user has since moved.
        if (mHasSavedViewCamera && cam.data() == mSavedViewCamera.data()) {
            cam->setLocalPos(mSavedViewCameraPos);
            cam->setLocalRot(mSavedViewCameraRot);
            cam->update(0);
        }
        mHasSavedViewCamera = false;
        mSavedViewCamera.clear();
        return;
    }
    if (!mHasSavedViewCamera || mSavedViewCamera.data() != cam.data()) {
        mSavedViewCamera = cam;
        mSavedViewCameraPos = cam->getLocalPos();
        mSavedViewCameraRot = cam->getLocalRot();
        mHasSavedViewCamera = true;
    }
    cam->setLocalPos(mCameraPos);
    cam->setLocalRot(mCameraRot);
    cam->update(0);
}

// --- play transitions ------------------------------------------------------

SceneNodePtr AvatarPossession::firstAvatar(const SceneNodePtr &root)
{
    if (root.isNull()) return SceneNodePtr();
    // DEPTH-FIRST, children in order — byte for byte the walk Environment
    // registers avatars with (environment.cpp's registerAvatars), which is what
    // makes "the first avatar" ONE order and not two. The ROOT itself is never
    // an avatar wrapper.
    const int kids = root->childCount();
    for (int i = 0; i < kids; ++i) {
        SceneNode *raw = root->childAt(i);
        if (!raw) continue;
        const SceneNodePtr child = raw->sharedFromThis();
        if (child->hasAvatarComponent()) return child;
        if (auto hit = firstAvatar(child)) return hit;
    }
    return SceneNodePtr();
}

void AvatarPossession::onPlayStarted()
{
    mFellBack = false;
    if (!mScene) return;

    const ScenePlayMode mode = mScene->getPlayMode();
    if (mode != ScenePlayMode::ThirdPerson) {
        // `explorer` and `camera` possess nothing. `camera` is the existing
        // active-camera behaviour and needs no help from here.
        unpossess();
        return;
    }

    const SceneNodePtr avatar = firstAvatar(mScene->getRootNode());
    if (avatar.isNull()) {
        // §8.5: "A scene with third-person and no avatar falls back to explorer
        // AND SAYS SO IN THE LOG. Silently doing nothing is the failure mode
        // that costs an hour of debugging." The serialized setting is NOT
        // rewritten — the user's choice survives adding a character later.
        mFellBack = true;
        unpossess();
        irisLog("playMode 'third-person': this scene contains no avatar — "
                "falling back to 'explorer' (free camera, nobody possessed). "
                "Drop a character in, or set scene.playMode('explorer').");
        return;
    }
    possess(avatar);
}

void AvatarPossession::onPlayStopped()
{
    mFellBack = false;
    unpossess();
    // Put the editor camera back where the arm found it. IDEMPOTENT: the second
    // stop finds mHasSavedCamera false and does nothing (gate P6).
    if (mHasSavedCamera && mScene) {
        if (const CameraNodePtr cam = mScene->camera) {
            cam->setLocalPos(mSavedCameraPos);
            cam->setLocalRot(mSavedCameraRot);
            cam->update(0);
        }
    }
    mHasSavedCamera = false;
    // The HOST's camera is restored by the next applyToViewCamera (which now
    // sees isPossessing() false); this only drops our claim on it if the host
    // never calls again — e.g. a viewport torn down mid-play.
    if (mHasSavedViewCamera) {
        if (const CameraNodePtr cam = mSavedViewCamera) {
            cam->setLocalPos(mSavedViewCameraPos);
            cam->setLocalRot(mSavedViewCameraRot);
            cam->update(0);
        }
        mHasSavedViewCamera = false;
        mSavedViewCamera.clear();
    }
}

}   // namespace iris
