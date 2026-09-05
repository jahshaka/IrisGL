/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/physics/avatarmovement.h"

#include "core/geometry/aabb.h"
#include "document/assets/mesh.h"
#include "document/scenegraph/meshnode.h"
#include "document/scenegraph/scenenode.h"

#include "btBulletCollisionCommon.h"
#include "BulletCollision/CollisionShapes/btCapsuleShape.h"

#include <algorithm>
#include <cmath>

namespace iris
{

namespace
{
inline btVector3 bt(const Vec3 &v) { return btVector3(v.x(), v.y(), v.z()); }
inline Vec3 fromBt(const btVector3 &v) { return Vec3(v.x(), v.y(), v.z()); }

/// Everything below this length is "no motion" — the sweep API returns no hit
/// for a zero-length cast (probe 4), so we simply skip those.
constexpr float kEpsMotion = 1e-6f;
}   // namespace

AvatarMovement::AvatarMovement()
{
    rebuildShape();
}

AvatarMovement::~AvatarMovement()
{
    delete mShape;
}

void AvatarMovement::setParams(const AvatarMovementParams &p)
{
    mParams = p;

    // CLAMPED, not refused. Every one of these is a number a user or a script
    // can type, and a negative speed or a zero-height capsule is a typo whose
    // only useful answer is the nearest sane value — which the verb then
    // reports back, so the caller sees what actually took.
    mParams.walkSpeed           = std::max(0.0f, mParams.walkSpeed);
    mParams.runSpeed            = std::max(0.0f, mParams.runSpeed);
    mParams.maxAcceleration     = std::max(0.01f, mParams.maxAcceleration);
    mParams.brakingDeceleration = std::max(0.0f, mParams.brakingDeceleration);
    mParams.groundFriction      = std::max(0.0f, mParams.groundFriction);
    mParams.jumpVelocity        = std::max(0.0f, mParams.jumpVelocity);
    mParams.jumpCount           = std::max(0, mParams.jumpCount);
    mParams.coyoteTime          = std::max(0.0f, mParams.coyoteTime);
    mParams.jumpReArm           = std::max(0.0f, mParams.jumpReArm);
    mParams.airControl          = std::min(1.0f, std::max(0.0f, mParams.airControl));
    mParams.gravityScale        = std::max(0.0f, mParams.gravityScale);
    mParams.maxStepHeight       = std::max(0.0f, mParams.maxStepHeight);
    // 89 deg, not 90: a "walkable" vertical wall would make the floor probe
    // classify the side of a box as ground and the character would stand on
    // air beside it.
    mParams.walkableFloorAngle  = std::min(89.0f, std::max(0.0f, mParams.walkableFloorAngle));
    mParams.rotationRate        = std::max(0.0f, mParams.rotationRate);
    mParams.capsuleRadius       = std::max(0.01f, mParams.capsuleRadius);
    // A capsule is at minimum a sphere: total height >= 2 * radius.
    mParams.capsuleHeight       = std::max(2.0f * mParams.capsuleRadius, mParams.capsuleHeight);

    rebuildShape();
}

void AvatarMovement::rebuildShape()
{
    const float r = mParams.capsuleRadius;
    const float h = mParams.capsuleHeight;
    if (mShape && std::fabs(r - mShapeRadius) < 1e-6f && std::fabs(h - mShapeHeight) < 1e-6f)
        return;
    delete mShape;
    // btCapsuleShape takes the CYLINDER height; total = height + 2 * radius.
    mShape = new btCapsuleShape(r, std::max(0.0f, h - 2.0f * r));
    mShapeRadius = r;
    mShapeHeight = h;
}

void AvatarMovement::setMoveInput(const Vec3 &planar)
{
    Vec3 v(planar.x(), 0.0f, planar.z());
    const float len = v.length();
    if (len > 1.0f) v = v / len;
    mInput.move = v;
}

void AvatarMovement::clearInput()
{
    mInput = AvatarMovementInput();
}

void AvatarMovement::reset()
{
    mVelocity = Vec3();
    mTimeSinceGrounded = 0.0f;
    mJumpReArmTimer = 0.0f;
    mJumpsUsed = 0;
    mHasYaw = false;
    mInput = AvatarMovementInput();
    mState = AvatarLocomotionState();
}

bool AvatarMovement::isWalkable(const Vec3 &normal) const
{
    const float cosLimit = std::cos(mParams.walkableFloorAngle * 3.14159265358979f / 180.0f);
    return normal.y() >= cosLimit;
}

AvatarMovement::SweepHit AvatarMovement::sweep(btCollisionWorld *world, const Vec3 &from,
                                               const Vec3 &to) const
{
    SweepHit out;
    if (!world || !mShape) return out;
    const Vec3 delta = to - from;
    if (delta.lengthSquared() < kEpsMotion * kEpsMotion) return out;

    btTransform tFrom, tTo;
    tFrom.setIdentity();
    tTo.setIdentity();
    tFrom.setOrigin(bt(from));
    tTo.setOrigin(bt(to));

    btCollisionWorld::ClosestConvexResultCallback cb(tFrom.getOrigin(), tTo.getOrigin());
    world->convexSweepTest(mShape, tFrom, tTo, cb);
    // m_hitNormalWorld is UNINITIALISED when hasHit() is false (measured: ~1e16
    // components). Only ever read it inside this branch.
    if (cb.hasHit()) {
        out.hit = true;
        out.fraction = std::min(1.0f, std::max(0.0f, float(cb.m_closestHitFraction)));
        out.normal = fromBt(cb.m_hitNormalWorld).normalized();
    }
    return out;
}

namespace
{
/// The nearest WALKABLE hit along a sweep, not the nearest hit.
///
/// Bullet's ClosestConvexResultCallback answers "what did I touch first", and
/// for the floor probe that is the wrong question: crossing a ledge, the
/// capsule's first contact is the platform's top EDGE, whose normal is 60-odd
/// degrees off +Y, and reading that as "no floor" made a character go airborne
/// for one frame on every drop it should have stepped down. This keeps every
/// result and takes the closest one that is actually floor. Returning 1 from
/// addSingleResult (what AllHitsRayResultCallback does) stops Bullet narrowing
/// the cast to hits closer than the last one, which is what would prune the
/// real floor underneath the edge.
struct NearestWalkableCallback : public btCollisionWorld::ConvexResultCallback
{
    explicit NearestWalkableCallback(float cosLimit) : mCosLimit(cosLimit) {}

    btScalar addSingleResult(btCollisionWorld::LocalConvexResult &r,
                             bool normalInWorldSpace) override
    {
        btVector3 n = normalInWorldSpace
                          ? r.m_hitNormalLocal
                          : r.m_hitCollisionObject->getWorldTransform().getBasis()
                                * r.m_hitNormalLocal;
        anyHit = true;
        if (n.y() >= mCosLimit && r.m_hitFraction < fraction) {
            fraction = float(r.m_hitFraction);
            normal = n;
            walkableHit = true;
        }
        return btScalar(1.0);
    }

    bool  anyHit = false;
    bool  walkableHit = false;
    float fraction = 1.0f;
    btVector3 normal{0, 1, 0};

private:
    float mCosLimit;
};
}   // namespace

AvatarMovement::SweepHit AvatarMovement::sweepFloor(btCollisionWorld *world, const Vec3 &from,
                                                    const Vec3 &to) const
{
    SweepHit out;
    if (!world || !mShape) return out;
    const Vec3 delta = to - from;
    if (delta.lengthSquared() < kEpsMotion * kEpsMotion) return out;

    btTransform tFrom, tTo;
    tFrom.setIdentity();
    tTo.setIdentity();
    tFrom.setOrigin(bt(from));
    tTo.setOrigin(bt(to));

    NearestWalkableCallback cb(std::cos(mParams.walkableFloorAngle * 3.14159265358979f / 180.0f));
    world->convexSweepTest(mShape, tFrom, tTo, cb);
    if (cb.walkableHit) {
        out.hit = true;
        out.fraction = std::min(1.0f, std::max(0.0f, cb.fraction));
        out.normal = fromBt(cb.normal).normalized();
    }
    return out;
}

// STEP-UP. The spec (§6.1 step 5) names "the standard up/forward/down triple",
// and the literal capsule version of it DOES NOT WORK against a capsule's
// rounded bottom — measured, not assumed:
//
//   a capsule of radius 0.3 stopped against a 0.3 u step's face rests with its
//   centre 0.3 short of the face, so a lifted forward leg of one frame's
//   displacement (0.033 u at walkSpeed) leaves the bottom cap still OUTSIDE the
//   step's footprint. The down leg then contacts the step's top EDGE, and an
//   edge contact's normal is the direction from the edge to the sphere centre —
//   (-0.897, 0.442, 0), i.e. 63.7 deg from +Y. That is not walkable, so the
//   triple refuses EVERY step. Making the forward leg long enough to clear
//   (radius + skin + delta) does climb it, but it teleports the character a
//   whole capsule radius in one frame — a 10x speed burst on every step.
//
// So the forward leg is a RAY instead of a capsule sweep: cast down just in
// front of the capsule's surface to find what the character is about to walk
// into, and if its top is walkable and within maxStepHeight, LIFT the capsule
// onto it and let the ordinary move carry it forward at its own speed. Same
// three questions — is there headroom, what is in front, how high is it — with
// no speed burst and no edge-normal false negative.
bool AvatarMovement::tryStepUp(btCollisionWorld *world, Vec3 &centre, const Vec3 &remaining)
{
    if (!world || mParams.maxStepHeight <= 0.0f) return false;
    Vec3 forward(remaining.x(), 0.0f, remaining.z());
    if (forward.lengthSquared() < kEpsMotion * kEpsMotion) return false;
    forward = forward.normalized();

    const float half = mParams.capsuleHeight * 0.5f;
    const float feetY = centre.y() - half;

    // FORWARD: just past the capsule's own surface, so the probe is over the
    // obstacle rather than over the ground the character is standing on.
    const Vec3 probe = centre + forward * (mParams.capsuleRadius + kSkin * 2.0f);
    const Vec3 rayFrom(probe.x(), feetY + mParams.maxStepHeight, probe.z());
    const Vec3 rayTo(probe.x(), feetY - kSkin, probe.z());

    btCollisionWorld::ClosestRayResultCallback cb(bt(rayFrom), bt(rayTo));
    world->rayTest(bt(rayFrom), bt(rayTo), cb);
    // A ray that starts INSIDE the obstacle reports nothing, which is exactly
    // the answer for a step taller than maxStepHeight: the probe begins in the
    // solid and the climb is refused. That is why the ray starts at
    // feet + maxStepHeight and not higher.
    if (!cb.hasHit()) return false;

    const Vec3 normal = fromBt(cb.m_hitNormalWorld).normalized();
    if (!isWalkable(normal)) return false;

    const float topY = float(cb.m_hitPointWorld.y());
    const float rise = topY - feetY;
    if (rise <= kSkin || rise > mParams.maxStepHeight) return false;

    // UP: the headroom check. A step you cannot stand up on is not a step.
    const Vec3 lifted = centre + Vec3(0.0f, rise + kSkin, 0.0f);
    if (sweep(world, centre, lifted).hit) return false;

    centre = lifted;
    return true;
}

Vec3 AvatarMovement::moveAndSlide(btCollisionWorld *world, Vec3 centre, Vec3 delta,
                                  bool allowStepUp)
{
    Vec3 remaining = delta;
    Vec3 prevNormal;
    bool hasPrev = false;

    for (int i = 0; i < kMaxSlides; ++i) {
        const float len = remaining.length();
        if (len < kEpsMotion) break;

        SweepHit hit = sweep(world, centre, centre + remaining);
        if (!hit.hit) {
            centre = centre + remaining;
            break;
        }

        // The pre-contact pose and the WHOLE remaining displacement, kept for
        // the step-up retry below. Retrying from AFTER the advance is the
        // obvious mistake and it silently disables step-up: by then the
        // leftover displacement is a few thousandths of a unit, the lifted
        // forward sweep gains nothing, and every step reads as a wall.
        const Vec3 preContact = centre;
        const Vec3 fullRemaining = remaining;

        // Advance to the contact, minus a skin so the next sweep never starts
        // exactly inside the surface.
        const float advance = std::max(0.0f, hit.fraction * len - kSkin);
        const Vec3 dir = remaining / len;
        centre = centre + dir * advance;
        remaining = remaining - dir * advance;

        // A blocking, non-walkable hit while walking gets the up/forward/down
        // triple before it is accepted as a wall (§6.1 step 5).
        if (allowStepUp && !isWalkable(hit.normal) && hit.normal.y() > -0.1f) {
            Vec3 climbed = preContact;
            if (tryStepUp(world, climbed, fullRemaining)) {
                // Lifted onto the step. The move CONTINUES from up there at the
                // character's own speed — the whole point of not doing the
                // forward leg with the capsule.
                centre = climbed;
                remaining = Vec3(fullRemaining.x(), 0.0f, fullRemaining.z());
                continue;
            }
        }

        const Vec3 n = hit.normal;
        if (hasPrev && Vec3::dotProduct(n, prevNormal) < 0.0f) {
            // A CREASE: two planes facing each other. Projecting onto the
            // second would push us back into the first and the two would
            // oscillate; slide along their intersection instead.
            Vec3 crease = Vec3::crossProduct(n, prevNormal);
            const float cl = crease.length();
            if (cl > 1e-4f) {
                crease = crease / cl;
                remaining = crease * Vec3::dotProduct(remaining, crease);
            } else {
                remaining = Vec3();
            }
        } else {
            remaining = remaining - n * Vec3::dotProduct(remaining, n);
        }
        // The VELOCITY has to be projected too, or the next sub-step
        // re-accelerates straight back into the wall and the character
        // shudders against it instead of sliding along it.
        mVelocity = mVelocity - n * std::min(0.0f, Vec3::dotProduct(mVelocity, n));

        prevNormal = n;
        hasPrev = true;
    }
    return centre;
}

void AvatarMovement::stepOnce(btCollisionWorld *world, const SceneNodePtr &node, float dt,
                              float gravityY)
{
    // WORLD SPACE throughout: the collision world is world-space, so reading
    // and writing anything else here would need a frame conversion per sweep
    // and would break the moment the wrapper gets a moving parent.
    const float half = mParams.capsuleHeight * 0.5f;
    const Vec3 feet = node->getGlobalPosition();
    Vec3 centre = feet + Vec3(0.0f, half, 0.0f);

    // ---- timers ----------------------------------------------------------
    if (mJumpReArmTimer > 0.0f) mJumpReArmTimer = std::max(0.0f, mJumpReArmTimer - dt);
    if (!mState.grounded) mTimeSinceGrounded += dt;

    // ---- 1. consume the jump latch --------------------------------------
    const bool wantJump = mInput.jump;
    mState.jumpRequested = wantJump;
    if (wantJump) {
        const bool coyote = mTimeSinceGrounded <= mParams.coyoteTime;
        const bool haveJumps = mJumpsUsed < mParams.jumpCount;
        if (mJumpReArmTimer <= 0.0f && haveJumps && (mState.grounded || coyote || mJumpsUsed > 0)) {
            mVelocity = Vec3(mVelocity.x(), mParams.jumpVelocity, mVelocity.z());
            mState.grounded = false;
            mState.mode = AvatarMovementMode::Falling;
            ++mJumpsUsed;
            mJumpReArmTimer = mParams.jumpReArm;
            // Past the coyote window immediately: a jump consumes the grace,
            // it does not keep it alive for a second one.
            mTimeSinceGrounded = mParams.coyoteTime + 1.0f;
        }
        // CLEARED UNCONDITIONALLY (§5): a held key that could not jump must
        // not stay latched until it can, or it fires the instant you land.
        mInput.jump = false;
    }

    // ---- 2/3. accelerate -------------------------------------------------
    const float targetSpeed = mInput.sprint ? mParams.runSpeed : mParams.walkSpeed;
    const Vec3 desired = mInput.move * targetSpeed;
    const bool hasInput = mInput.move.lengthSquared() > 1e-8f;
    Vec3 planar(mVelocity.x(), 0.0f, mVelocity.z());

    if (mState.mode == AvatarMovementMode::Walking) {
        if (hasInput) {
            // GROUND FRICTION, applied the way a CMC applies it: it steers the
            // existing velocity toward the input direction WITHOUT changing its
            // magnitude. That is what "lateral velocity kill on slopes and
            // strafe reversal" means (§6.2) — and it is why it must not be a
            // plain damping term. A damping term costs speed every step even at
            // the target, so the character never reaches walkSpeed and M1's
            // 5%-of-walkSpeed gate fails by exactly the friction coefficient.
            const Vec3 accelDir = mInput.move.normalized();
            const float speed = planar.length();
            const float f = std::min(1.0f, mParams.groundFriction * dt);
            planar = planar - (planar - accelDir * speed) * f;

            Vec3 toTarget = desired - planar;
            const float d = toTarget.length();
            const float maxDelta = mParams.maxAcceleration * dt;
            planar = (d <= maxDelta) ? desired : planar + toTarget / d * maxDelta;
        } else {
            // No input: braking, and friction on TOP of it — with nothing to
            // steer toward, friction is pure decay and the two together are
            // what "stopping does not feel like starting" means.
            const float sp = planar.length();
            const float brake = (mParams.brakingDeceleration + mParams.groundFriction * sp) * dt;
            planar = (sp <= brake || sp < 1e-6f) ? Vec3() : planar * ((sp - brake) / sp);
        }
        mVelocity = Vec3(planar.x(), std::min(0.0f, mVelocity.y()), planar.z());
    } else {
        if (hasInput) {
            Vec3 toTarget = desired - planar;
            const float d = toTarget.length();
            const float maxDelta = mParams.maxAcceleration * mParams.airControl * dt;
            planar = (d <= maxDelta) ? desired : planar + toTarget / d * maxDelta;
        }
        const float vy = mVelocity.y() + gravityY * mParams.gravityScale * dt;
        mVelocity = Vec3(planar.x(), vy, planar.z());
    }

    // ---- 4/5. move ------------------------------------------------------
    const bool walking = mState.mode == AvatarMovementMode::Walking;
    centre = moveAndSlide(world, centre, mVelocity * dt, walking);

    // ---- 6. floor check -------------------------------------------------
    // The probe starts slightly ABOVE the capsule so it never begins in
    // contact (probe 3: a contact start returns fraction 0, which is a correct
    // normal but a useless distance). While Walking it reaches a whole
    // maxStepHeight down, which is what re-plants the capsule after a small
    // drop instead of launching it (§6.1 step 5, the step-DOWN half).
    const float lift = kSkin * 2.0f;
    const float reach = walking ? (mParams.maxStepHeight + lift * 2.0f) : (lift * 3.0f);
    const Vec3 probeFrom = centre + Vec3(0.0f, lift, 0.0f);
    SweepHit floor = sweepFloor(world, probeFrom, probeFrom + Vec3(0.0f, -reach, 0.0f));

    const bool rising = mVelocity.y() > 1e-4f;
    bool grounded = false;
    if (floor.hit && !rising) {
        grounded = true;
        const float drop = floor.fraction * reach;
        // Snap so the capsule rests kSkin above the contact.
        centre = probeFrom + Vec3(0.0f, -(std::max(0.0f, drop - kSkin)), 0.0f);
        if (mVelocity.y() < 0.0f) mVelocity = Vec3(mVelocity.x(), 0.0f, mVelocity.z());
    }

    if (grounded) {
        mState.mode = AvatarMovementMode::Walking;
        mTimeSinceGrounded = 0.0f;
        mJumpsUsed = 0;
    } else {
        mState.mode = AvatarMovementMode::Falling;
    }
    mState.grounded = grounded;

    // ---- 7 (already consumed) / 8. orient -------------------------------
    Vec3 planarVel(mVelocity.x(), 0.0f, mVelocity.z());
    if (mParams.orientRotationToMovement && planarVel.lengthSquared() > 1e-6f) {
        // Forward is the node's -Z (the document's convention), so the yaw
        // that points -Z along d is atan2(dx, -dz).
        const float targetYaw = std::atan2(planarVel.x(), -planarVel.z()) * 180.0f / 3.14159265358979f;
        const Quat target = Quat::fromAxisAndAngle(Vec3(0, 1, 0), targetYaw);
        if (!mHasYaw) {
            mYaw = target;
            mHasYaw = true;
        } else {
            // Shortest-arc step, capped at rotationRate * dt.
            float dot = mYaw.x() * target.x() + mYaw.y() * target.y() + mYaw.z() * target.z()
                        + mYaw.scalar() * target.scalar();
            Quat to = target;
            if (dot < 0.0f) {
                to = Quat(-target.scalar(), -target.x(), -target.y(), -target.z());
                dot = -dot;
            }
            const float angle = 2.0f * std::acos(std::min(1.0f, std::max(-1.0f, dot)))
                                * 180.0f / 3.14159265358979f;
            const float maxStep = mParams.rotationRate * dt;
            const float t = (angle <= maxStep || angle < 1e-4f) ? 1.0f : maxStep / angle;
            mYaw = Quat::slerp(mYaw, to, t).normalized();
        }
        node->setGlobalRot(mYaw);
    }

    // ---- write the one transform this component owns --------------------
    node->setGlobalPos(centre - Vec3(0.0f, half, 0.0f));

    // ---- 9. publish ------------------------------------------------------
    mState.speed = planarVel.length();
    mState.moveInput = mInput.move;
    mState.verticalVelocity = mVelocity.y();
}

void AvatarMovement::step(btCollisionWorld *world, const SceneNodePtr &node, float dt,
                          float gravityY)
{
    if (!node || dt <= 0.0f) return;
    rebuildShape();

    // Fixed sub-stepping, §6.1: at most 8 x 0.05 s, LEFTOVER DROPPED. An
    // unbounded catch-up after a shader-compile hitch is how characters tunnel
    // through floors; a bounded one only makes them fall behind for a frame.
    float remaining = dt;
    for (int i = 0; i < kMaxSubSteps && remaining > kEpsMotion; ++i) {
        const float h = std::min(kMaxSubStep, remaining);
        stepOnce(world, node, h, gravityY);
        remaining -= h;
    }
}

// ---------------------------------------------------------------------------

namespace
{
/// Walks the subtree, merging every mesh's local AABB CORNERS expressed in the
/// wrapper's frame. Corners (not just min/max) because the lower-half test
/// below needs individual points, and because a rotated child's AABB corners
/// are the only cheap approximation of its real footprint.
void mergeNodeBounds(SceneNode *n, const Mat4 &rootInv, AABB &full, QVector<Vec3> &corners)
{
    if (!n) return;
    if (n->getSceneNodeType() == SceneNodeType::Mesh) {
        auto *meshNode = static_cast<MeshNode *>(n);
        if (auto mesh = meshNode->getMesh()) {
            const Mat4 toRoot = rootInv * n->getGlobalTransform();
            const AABB local = mesh->getAABB();
            const Vec3 mn = local.getMin();
            const Vec3 mx = local.getMax();
            for (int c = 0; c < 8; ++c) {
                const Vec3 p((c & 1) ? mx.x() : mn.x(),
                             (c & 2) ? mx.y() : mn.y(),
                             (c & 4) ? mx.z() : mn.z());
                const Vec3 w = toRoot * p;
                full.merge(w);
                corners.append(w);
            }
        }
    }
    const int kids = n->childCount();
    for (int i = 0; i < kids; ++i) {
        SceneNode *c = n->childAt(i);
        if (!c) continue;
        mergeNodeBounds(c, rootInv, full, corners);
    }
}
}   // namespace

void AvatarMovement::fitCapsuleToNode(const SceneNodePtr &node)
{
    if (!node || !mParams.capsuleAuto) return;

    // Everything is measured in the WRAPPER's frame: the wrapper is what the
    // component moves, and a capsule expressed in any other frame would drift
    // the moment the wrapper is rotated by orient-to-movement.
    const Mat4 rootInv = node->getGlobalTransform().inverted();

    AABB full;
    full.setNegativeInfinity();
    QVector<Vec3> corners;

    mergeNodeBounds(node.data(), rootInv, full, corners);

    if (corners.isEmpty()) return;   // nothing to measure — the defaults stand

    const float minY = full.getMin().y();
    const float maxY = full.getMax().y();
    const float height = maxY - minY;
    if (!(height > 0.01f)) return;

    // R4: the radius comes from the planar footprint of the LOWER HALF only.
    // A T-posed character's full width is its ARM SPAN, and a capsule fitted to
    // it is a barrel that fits through no door.
    const float midY = minY + height * 0.5f;
    float halfX = 0.0f, halfZ = 0.0f;
    bool any = false;
    for (const Vec3 &p : corners) {
        if (p.y() > midY) continue;
        halfX = std::max(halfX, std::fabs(p.x()));
        halfZ = std::max(halfZ, std::fabs(p.z()));
        any = true;
    }
    if (!any) {
        for (const Vec3 &p : corners) {
            halfX = std::max(halfX, std::fabs(p.x()));
            halfZ = std::max(halfZ, std::fabs(p.z()));
        }
    }

    AvatarMovementParams p = mParams;
    p.capsuleHeight = height;
    // Never wider than the capsule is tall: a capsule whose radius exceeds
    // half its height is a sphere and steps stop working.
    p.capsuleRadius = std::min(std::max(halfX, halfZ), height * 0.5f * 0.9f);
    const bool keepAuto = mParams.capsuleAuto;
    setParams(p);
    mParams.capsuleAuto = keepAuto;
}

}   // namespace iris
