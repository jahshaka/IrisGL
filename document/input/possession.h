/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_POSSESSION_H
#define IRIS_POSSESSION_H

// iris::AvatarPossession — the ONE possession slot, the input routing it
// decides, and the spring-arm follow camera that rides with it
// (AVATAR_LOCOMOTION_SPEC §8.4 / §8.5, Stage 3).
//
// THE SHAPE IS scene.setActiveCamera's, deliberately (§8.4): one nullable
// choice per scene, validated by the only object that can resolve a guid to a
// node, RUNTIME ONLY — which avatar you were driving is not a property of the
// document and is never written to the file. `playMode` IS serialized; the
// possession it produces is not.
//
// WHAT POSSESSION IS. Unreal separates the Pawn from the Controller; we keep
// the routing decision and throw the actors away. There is one local player, so
// there is one slot: the possessed avatar is the ONE consumer of InputSystem
// state, and every other registered avatar keeps stepping with ZERO input (§8.4
// — "a frozen second avatar reads as a broken rig"). possess(B) on an already
// possessed A unpossesses A first, and unpossessing CLEARS the released
// avatar's input rather than leaving it frozen at its last value: a W held at
// the moment of a switch would otherwise walk A forever (gate P5).
//
// WHERE THE JUMP LATCH IS CLEARED. Here, in routeInput — which runs as the
// first half of the movement step — and nowhere else. `possess()`/`unpossess()`
// never touch InputSystem: a verb that swallowed a pending jump would make the
// latch depend on when a script happened to call it. What the route does is
// TRANSFER the latch: InputSystem::consumeJump() -> AvatarMovement::requestJump(),
// after which the component's own step consumes it (§5).
//
// WHY THE FOLLOW CAMERA IS A SPRING ARM ON THE WRAPPER, NOT A SOCKET RIDER
// (§8.5, the one non-obvious call). The `shoulder` socket ships and a camera
// attached to it works — but a socket rides a BONE, and a shoulder bone bobs
// with every footfall. Correct for over-the-shoulder aiming, wrong for a smooth
// third-person follow: the camera visibly bounces and reads as a physics bug.
// So the arm is anchored to the avatar WRAPPER's transform (which the movement
// component writes, and which does not bob) and the socket is read ONCE, at
// possess time, purely as the OFFSET REFERENCE — how high and how far to the
// side this particular character's shoulder sits. Cost: identical.
//
// WHY THIS IS DOCUMENT-SIDE. Every gate in §11's Stage 3 row — camera-relative
// input, auto-possess order, the explorer fallback, the possess-while-held-W
// case — has to be assertable with no window, no display and no engine. The
// arm therefore COMPUTES its pose here (Scene::update drives it, right after
// the movement step, so it never lags the character by a frame) and writes
// `Scene::camera`, which the document already owns and already updates.
//
// ...and the pose is APPLIED by the host, through applyToViewCamera. That is
// not indirection for its own sake: `Scene::camera` and the camera a viewport
// actually renders through are not always the same CameraNode in this tree
// (measured — see the .cpp), and only the host knows which one it draws with.

#include <QString>

#include "core/math/quat.h"
#include "core/math/vec.h"
#include "irisglfwd.h"

namespace iris
{

/// What Play does with this scene (§8.5). SERIALIZED with the scene, beside
/// `activeCamera` — deliberately NOT a World Mode row: `world.modeTable` is the
/// SCALABILITY registry (quality tiers), and a gameplay decision that followed
/// low/medium/high/epic would be nonsense.
enum class ScenePlayMode : int
{
    Explorer = 0,      ///< free camera, nobody possessed — the behaviour Play always had
    ThirdPerson = 1,   ///< auto-possess the first avatar, install the follow camera
    Camera = 2         ///< render through scene.activeCamera, nobody possessed
};

/// Stable strings for the file and the verb. The enum ints must stay free to be
/// reordered — the same rule giMode/giQuality are written under.
const char *playModeName(ScenePlayMode mode);
/// Case-insensitive; false (and `out` untouched) when `name` names no mode.
bool playModeFromName(const QString &name, ScenePlayMode &out);

/// The spring arm. Every field is in world units / degrees.
struct FollowCameraParams
{
    float armLength = 4.0f;        ///< how far behind the pivot the camera sits
    float heightOffset = 1.6f;     ///< pivot height above the wrapper origin (the FEET)
    float lateralOffset = 0.0f;    ///< pivot offset along the camera's right axis
    float minPitch = -70.0f;       ///< looking up
    float maxPitch = 75.0f;        ///< looking down
    /// Degrees per unit of accumulated Look delta. The Look producer hands in
    /// raw pixels (playback.cpp), so this is degrees per pixel.
    float yawSensitivity = 0.25f;
    float pitchSensitivity = 0.25f;
};

class AvatarPossession
{
public:
    /// `scene` is the owner and outlives this object; never null in practice
    /// (Scene constructs it), tolerated as null so the class is unit-testable.
    explicit AvatarPossession(Scene *scene);
    ~AvatarPossession();

    AvatarPossession(const AvatarPossession &) = delete;
    AvatarPossession &operator=(const AvatarPossession &) = delete;

    // ---- the slot ---------------------------------------------------------

    /// Takes control of `node`. Refused (false, nothing changed) when the node
    /// is null or carries no avatar component. Implicitly UNPOSSESSES whatever
    /// was possessed first — including clearing its input (§8.4, gate P5).
    /// Possessing the already-possessed avatar is a no-op that returns true and
    /// does NOT re-read the socket reference or reset the camera angles.
    bool possess(const SceneNodePtr &node);
    /// Releases the current avatar and clears its input. True when something
    /// was released; false (and harmless) when nothing was possessed — which is
    /// what makes a redundant stop free (§8.3 rule 1, gate P6).
    bool unpossess();
    /// Null when nobody is possessed, or when the possessed node has since been
    /// deleted (the slot holds a WEAK reference for exactly that reason).
    SceneNodePtr possessed() const;
    /// The possessed node's guid, or an empty string. Cheaper than possessed()
    /// for the verbs, and it survives the node dying.
    QString possessedGuid() const;
    bool isPossessing() const { return !possessed().isNull(); }

    // ---- the follow camera ------------------------------------------------

    const FollowCameraParams &follow() const { return mFollow; }
    void setFollow(const FollowCameraParams &p);

    /// Degrees. Yaw is the camera's heading (0 = looking down world -Z, the
    /// scene's forward); pitch is clamped to [minPitch, maxPitch].
    float yaw() const { return mYaw; }
    float pitch() const { return mPitch; }
    void setYaw(float degrees);
    void setPitch(float degrees);

    /// Where the arm puts the camera for the possessed avatar's CURRENT pose,
    /// and the rotation that looks back down it. Both are defined only while
    /// possessing; they return the last computed values otherwise.
    Vec3 cameraPosition() const { return mCameraPos; }
    Quat cameraRotation() const { return mCameraRot; }

    /// The planar world direction the possessed avatar walks for a stick that
    /// reads (x, y) — i.e. the camera-relative mapping, in one place so the
    /// gates can assert it without a scene. `y` is FORWARD (world -Z at yaw 0).
    Vec3 moveToWorld(float x, float y) const;

    // ---- the per-frame halves ---------------------------------------------
    //
    // Two calls, on either side of the physics/movement step, because the
    // camera must follow the pose the step just produced. Scene::update drives
    // both; nothing else should.

    /// Look -> the arm's angles; Move -> the possessed avatar's camera-relative
    /// planar intent; Sprint -> its sprint; the Jump LATCH is transferred (§5).
    /// A no-op with nothing possessed — and it deliberately does not drain the
    /// look accumulator in that case, so a mouse move made a frame before
    /// possession is not silently eaten.
    void routeInput(float dt);
    /// Recomputes the arm from the wrapper's new transform and writes
    /// `Scene::camera`. A no-op with nothing possessed.
    void updateFollowCamera();

    /// Applies the arm to the camera the HOST renders through, and gives that
    /// camera back when possession ends.
    ///
    /// It exists because `Scene::camera` and the camera a viewport actually
    /// renders are NOT always the same CameraNode — measured on this tree, not
    /// assumed (see the .cpp). The document cannot know which one a host draws
    /// with, so the host hands it in, once per frame, after the playback update.
    /// Safe to call every frame whether or not anything is possessed: with
    /// nothing possessed it restores once and then does nothing.
    void applyToViewCamera(const CameraNodePtr &cam);

    // ---- play transitions (§8.3 rule 1 — these hang off the EDGE) ---------

    /// The rising edge of Scene::setPlaying. In `third-person` it possesses the
    /// FIRST avatar in document order (depth-first from the root — the same
    /// order Environment registers them in, gate P3); with no avatar in the
    /// scene it logs the fallback and possesses nothing (gate P4). Every other
    /// mode possesses nothing.
    void onPlayStarted();
    /// The falling edge. Unpossesses, restores the editor camera to the pose it
    /// had when the arm took it over, and forgets the socket reference.
    /// IDEMPOTENT: called twice it finds nothing to do the second time, which
    /// is the whole of gate P6.
    void onPlayStopped();

    /// True when the last onPlayStarted() wanted `third-person` and there was
    /// no avatar to possess. Cleared by the next transition. The log line is
    /// the user-visible half; this is the assertable one.
    bool fellBackToExplorer() const { return mFellBack; }

    /// The first avatar in DOCUMENT ORDER (depth-first, children in order),
    /// or null. Public because "which one is first" is the deterministic claim
    /// gate P3 asserts, and a test should be able to ask directly.
    static SceneNodePtr firstAvatar(const SceneNodePtr &root);

private:
    /// Reads the `shoulder` socket ONCE (bind pose — no engine needed) off the
    /// first rigged mesh in the subtree and turns it into heightOffset /
    /// lateralOffset. Falls back to the movement capsule's height when the
    /// character has no shoulder socket, and to the struct defaults when it has
    /// no capsule either. Never per-frame: that is the bob this avoids.
    void captureOffsetReference(const SceneNodePtr &node);

    Scene *mScene = nullptr;
    SceneNodeWPtr mPossessed;
    QString mPossessedGuid;

    FollowCameraParams mFollow;
    float mYaw = 0.0f;
    float mPitch = 12.0f;      ///< a slight look-down, the third-person default

    Vec3 mCameraPos;
    Quat mCameraRot;

    /// The editor camera's pose when the arm took it over, so stop puts it
    /// back. `mHasSavedCamera` is what makes the restore idempotent.
    Vec3 mSavedCameraPos;
    Quat mSavedCameraRot;
    bool mHasSavedCamera = false;

    /// The same, for the HOST's render camera (applyToViewCamera). Held
    /// strongly: it is the node we owe a restore to, and it must not vanish
    /// between the last possessed frame and the first unpossessed one.
    CameraNodePtr mSavedViewCamera;
    Vec3 mSavedViewCameraPos;
    Quat mSavedViewCameraRot;
    bool mHasSavedViewCamera = false;

    bool mFellBack = false;
};

}   // namespace iris

#endif   // IRIS_POSSESSION_H
