/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef AVATAR_MOVEMENT_H
#define AVATAR_MOVEMENT_H

// iris::AvatarMovement — the character movement component
// (SPECS/AVATAR_LOCOMOTION_SPEC.md §6, decision L2).
//
// OURS, modeled on Unreal's CharacterMovementComponent. Bullet is the
// COLLISION-QUERY LAYER ONLY: this class owns a btCapsuleShape it uses purely
// as a CAST shape and asks the existing btCollisionWorld questions through
// convexSweepTest. It adds NOTHING to the world — no ghost object, no rigid
// body, no broadphase proxy — which is why deleting an avatar mid-play cannot
// leave a stale broadphase entry (gate M7). `btKinematicCharacterController`
// is ruled out by L2 and is not used, wrapped or referenced.
//
// It writes exactly one thing: the avatar wrapper node's transform.
//
// ---------------------------------------------------------------------------
// THE PROBE THAT HAD TO RUN FIRST (§13, "nobody in this codebase has ever
// called convexSweepTest"). Measured against the world Environment builds and
// the Ground PhysicsHelper builds, before a line of the step order was written:
//
//   * A capsule swept DOWN onto the default Ground (btStaticPlaneShape(0,1,0))
//     hits at EXACTLY the geometric fraction — 0.5250 for a 1.8 u capsule
//     dropped from centre y=3 to y=-1 — with normal (0,1,0). Bullet's convex
//     cast already accounts for the capsule's own radius, so no manual
//     shrink is needed, and the capsule's collision margin does NOT inflate it.
//   * A horizontal sweep into a static box stops one radius short of the face
//     and reports the face normal.
//   * A sweep that STARTS IN CONTACT with the floor reports hasHit with
//     fraction 0 and a correct normal — usable for the floor probe, but it
//     means "advance by the fraction" is a no-op in that state, so the floor
//     probe always starts from slightly ABOVE the capsule's resting height.
//   * A HORIZONTAL sweep from the exact resting pose does NOT report the floor
//     (tangential motion is not a hit) at any gap from 0 to 0.02 — walking on
//     a plane needs no skin lift. Checked because a spurious floor hit here
//     would have frozen every step.
//   * A ZERO-LENGTH sweep returns no hit and does not crash (a sub-step with
//     no displacement is safe).
//   * On a 60 deg rotated box the hit normal reports 60.00 deg from +Y, so the
//     walkable-floor classification can be read straight off m_hitNormalWorld.
//   * The callback's m_collisionFilterMask really filters.
//   * TRAP: when hasHit() is false, m_hitNormalWorld is UNINITIALISED GARBAGE
//     (measured: components around 1e16). Never read it unguarded.
// ---------------------------------------------------------------------------
//
// CONVENTIONS, stated once because getting either wrong is invisible:
//
//   * `moveInput` is a PLANAR WORLD VECTOR on the unit disc — (x, 0, z) in
//     world space, its Y component ignored. It is NOT a 2D stick: the input
//     layer's {x, y} axes map to it as (x, 0, -y), because forward is world -Z
//     (the scene's forward, the same axis the document's lights point down
//     from), and the possession layer (Stage 3) rotates that by the camera yaw
//     before handing it here. A headless test writes world-space intent
//     directly. Both write the same struct, which is what makes `avatar.input`
//     and the keyboard producer interchangeable.
//   * The wrapper node's ORIGIN IS THE CHARACTER'S FEET. The capsule's centre
//     is origin + (0, capsuleHeight/2, 0).
//   * Bullet's btCapsuleShape(radius, height) takes the CYLINDER height, so it
//     is constructed with (capsuleHeight - 2*radius).

#include "core/math/quat.h"
#include "core/math/vec.h"
#include "irisglfwd.h"

class btCollisionWorld;
class btCapsuleShape;

namespace iris
{

/// v1 has exactly two modes (§6.1 step 2). No swimming, flying, nav-walking or
/// perch logic exists, deliberately.
enum class AvatarMovementMode : int
{
    Walking = 0,
    Falling = 1
};

/// The §6.2 knob set. Plain data: serialized by src/io, read and written by
/// `avatar.movement` / `avatar.setMovement`, never wired by the user.
struct AvatarMovementParams
{
    float walkSpeed = 2.0f;              ///< u/s. The blend space's mid sample.
    float runSpeed = 5.5f;               ///< u/s. Used while `sprint` is held.
    float maxAcceleration = 20.0f;       ///< u/s^2
    float brakingDeceleration = 25.0f;   ///< u/s^2, applied when input is zero
    float groundFriction = 8.0f;         ///< lateral velocity kill, 1/s
    float jumpVelocity = 6.0f;           ///< u/s, applied as an instant Y set
    int   jumpCount = 1;                 ///< 1 = single jump, 2 = double, ...
    float coyoteTime = 0.12f;            ///< s of grace after leaving the floor
    float jumpReArm = 0.1f;              ///< s before a held key may fire again
    float airControl = 0.35f;            ///< 0..1 scale on maxAcceleration
    float gravityScale = 1.0f;           ///< scales the WORLD's gravity
    float maxStepHeight = 0.4f;          ///< u the capsule can step up/down
    float walkableFloorAngle = 45.0f;    ///< deg from +Y
    bool  orientRotationToMovement = true;
    float rotationRate = 540.0f;         ///< deg/s
    /// Capsule dimensions. `capsuleAuto` means "re-derive from the mesh bounds
    /// at spawn / at load"; setting either dimension explicitly clears it.
    bool  capsuleAuto = true;
    float capsuleRadius = 0.3f;
    float capsuleHeight = 1.8f;          ///< TOTAL height, caps included
};

/// The §5 parameter contract — the five values the component publishes per
/// avatar per fixed step, plus the mode. `avatar.locomotionState` (Stage 4)
/// reports exactly this; the state machine reads exactly this.
struct AvatarLocomotionState
{
    float speed = 0.0f;                  ///< PLANAR speed, Y stripped, >= 0
    Vec3  moveInput;                     ///< x/z of the consumed intent (y = 0)
    bool  grounded = false;
    float verticalVelocity = 0.0f;
    bool  jumpRequested = false;         ///< the latch, as seen this step
    AvatarMovementMode mode = AvatarMovementMode::Falling;
};

/// What the input layer (Stage 1) or the possession layer (Stage 3) writes.
/// `jump` is a LATCH: set by the producer, cleared unconditionally by the
/// component in the step that consumes it, so a held key cannot re-fire.
struct AvatarMovementInput
{
    Vec3 move;          ///< planar world intent, clamped to the unit disc
    bool jump = false;  ///< one-shot latch
    bool sprint = false;
};

class AvatarMovement
{
public:
    AvatarMovement();
    ~AvatarMovement();

    AvatarMovement(const AvatarMovement &) = delete;
    AvatarMovement &operator=(const AvatarMovement &) = delete;

    // ---- knobs ------------------------------------------------------------
    const AvatarMovementParams &params() const { return mParams; }
    /// Replaces the whole set. Values are CLAMPED to sane ranges rather than
    /// refused (a negative walk speed is a typo, not a feature) — the resolved
    /// set is what `avatar.setMovement` reports back.
    void setParams(const AvatarMovementParams &p);

    // ---- input ------------------------------------------------------------
    const AvatarMovementInput &input() const { return mInput; }
    /// Sets the planar intent. Clamped to the unit disc here, once, so no
    /// producer can hand in a diagonal that walks 1.41x.
    void setMoveInput(const Vec3 &planar);
    void setSprint(bool on) { mInput.sprint = on; }
    /// Latches a jump. Cleared by the step that consumes it.
    void requestJump() { mInput.jump = true; }
    /// Zeroes the whole input struct — what possession does to the avatar it
    /// releases, so a held W cannot walk it forever (§8.4).
    void clearInput();

    // ---- state ------------------------------------------------------------
    const AvatarLocomotionState &state() const { return mState; }
    Vec3 velocity() const { return mVelocity; }
    /// Back to a settled Falling avatar with no velocity and no input. Called
    /// when play stops.
    void reset();

    // ---- the step ---------------------------------------------------------
    /// One FRAME. Sub-steps internally at most 8 x 0.05 s (§6.1) and drops the
    /// leftover rather than integrating it at once. `world` may be null (no
    /// physics world yet): the component then only integrates gravity and
    /// never claims to be grounded, which is what a document-only host sees.
    ///
    /// `gravityY` is the WORLD's gravity, handed in rather than read off the
    /// world: the query layer this component needs is btCollisionWorld, which
    /// has no gravity — only btDynamicsWorld does — and taking a dynamics
    /// world here just to read one float would tie a component that simulates
    /// nothing to the solver (L2: Bullet is the COLLISION-QUERY LAYER ONLY).
    /// `gravityScale` multiplies it; the world stays the one gravity source.
    /// One fixed step of `dt` (the document's SimulationClock grid, 1/60 s —
    /// Scene::advance calls this once per grid step through the Environment).
    void step(btCollisionWorld *world, const SceneNodePtr &node, float dt,
              float gravityY = -10.0f);

    /// Derives capsuleRadius/capsuleHeight from the node's subtree bounds
    /// (R4: the radius comes from the planar footprint of the LOWER HALF of
    /// the bind-pose bounds — a T-posed character's full width is its arm
    /// span, and using it makes every avatar a barrel). No-op unless
    /// `capsuleAuto`. Safe on a node with no meshes: the defaults stand.
    void fitCapsuleToNode(const SceneNodePtr &node);

    /// Slide iterations per step (§6.1 step 4).
    static constexpr int   kMaxSlides = 4;
    /// The skin the sweep backs off by, in world units.
    static constexpr float kSkin = 0.005f;

private:
    struct SweepHit
    {
        bool  hit = false;
        float fraction = 1.0f;
        Vec3  normal;
    };

    SweepHit sweep(btCollisionWorld *world, const Vec3 &from, const Vec3 &to) const;
    /// The FLOOR probe: the nearest WALKABLE hit, which is not the nearest hit.
    /// See the callback in the .cpp — a ledge's top edge is always the closer
    /// contact and is never floor.
    SweepHit sweepFloor(btCollisionWorld *world, const Vec3 &from, const Vec3 &to) const;
    bool isWalkable(const Vec3 &normal) const;
    void rebuildShape();
    void stepOnce(btCollisionWorld *world, const SceneNodePtr &node, float dt, float gravityY);
    /// Sweep-and-slide `delta` from `centre`, returning the reached centre and
    /// updating `mVelocity` by projecting it onto each blocking plane.
    Vec3 moveAndSlide(btCollisionWorld *world, Vec3 centre, Vec3 delta, bool allowStepUp);
    /// The up / forward / down triple (§6.1 step 5). Returns true and writes
    /// `centre` when the obstacle was climbed.
    bool tryStepUp(btCollisionWorld *world, Vec3 &centre, const Vec3 &remaining);

    AvatarMovementParams  mParams;
    AvatarMovementInput   mInput;
    AvatarLocomotionState mState;

    Vec3  mVelocity;
    float mTimeSinceGrounded = 0.0f;   ///< coyote-time clock
    float mJumpReArmTimer = 0.0f;
    int   mJumpsUsed = 0;
    Quat  mYaw;                        ///< last orientation, held when still
    bool  mHasYaw = false;

    /// The CAST shape. Owned, never in the world. Rebuilt when the dimensions
    /// change.
    btCapsuleShape *mShape = nullptr;
    float mShapeRadius = 0.0f;
    float mShapeHeight = 0.0f;
};

}   // namespace iris

#endif   // AVATAR_MOVEMENT_H
