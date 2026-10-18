#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

// This class entails the simulated environment. That is, bullet utilized in iris.
// DO NOT use bullet specific types or functions in the main application, all objects should live here
// This includes any constraints, rigid bodies and bullet specific variables.
// If you need to use btVector3 etc somewhere, consider doing it here and deleting it after
// For example, all the rigid bodies in the scene are contained inside hashBodies

// See { bullet specific variables, bullet specific constraints }

#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "document/physics/physicshelper.h"
#include "document/scenegraph/nodegraph.h"

#include <QVector>
#include <QHash>
#include <QSet>

#include "btBulletDynamicsCommon.h"

#include "document/physics/physicshelper.h"

class btTypedConstraint;
class btCollisionShape;
class btRigidBody;
class btCollisionConfiguration;
class btDispatcher;
class btBroadphaseInterface;
class btConstraintSolver;
class btDynamicsWorld;
class btStridingMeshInterface;
class btGhostPairCallback;

namespace iris
{

enum class PickingHandleType : int
{
	None,
	LeftHand,
	RightHand,
	MouseButton
};

struct PickingHandle
{
	btRigidBody *activeRigidBodyBeingManipulated = nullptr;
	btTypedConstraint *activePickingConstraint = nullptr;
	int	activeRigidBodySavedState = 0;
	btVector3 constraintOldPickingPosition;
	btVector3 constraintHitPosition;
	btScalar constraintOldPickingDistance;
	PickingHandleType pickHandleType = PickingHandleType::None;
};

class Environment
{
public:

    Environment();
    ~Environment();

    /// THE PHYSICS STEP'S THREADS AND ITS ONE THREAD (ENGINE-UPGRADE-U4 stage 6, decision 23). Bullet's multi-threaded
    /// world (btDiscreteDynamicsWorldMt: the collision pairs and the simulation islands across Bullet's OWN task
    /// scheduler) steps on `threads` threads, the calling thread included — the host's share of the box's ONE core
    /// budget (beside Ogre's pool, the render and game threads and the dedicated ones: sceneworkers::physicsThreads),
    /// fixed for the process. Called ONCE, on the GAME THREAD, before the first world is stepped:
    ///
    /// THE THREAD-INDEX TRAP. Bullet hands out thread indices from one global counter on first use (whoever asks first
    /// is index 0, "main"; at most 64, never given back), and the MT dispatcher's per-thread manifold arrays — sized by
    /// the scheduler's thread count when the world is built — are indexed by it (btCollisionDispatcherMt.cpp:31,73).
    /// So index 0 must be the game thread (this call takes it: btSetTaskScheduler refuses any other), Bullet's own
    /// workers take 1..threads-1, and NO other thread may ever step or ray-test a world: a Qt-pool or render thread
    /// taking an index would push a worker past the arrays. stepSimulation dies (qFatal) on any other thread.
    ///
    /// A process that never binds (a tool, a test with no physics frame) builds the single-threaded world.
    static void bindGameThread(int threads);
    /// The thread count of the bound scheduler (1 when unbound, or in a build without BT_THREADSAFE).
    static int taskThreads();
    /// True on the bound game thread (and in a process that never bound).
    static bool onGameThread();
    /// The world type this build creates once bound: true = btDiscreteDynamicsWorldMt.
    static bool multiThreadedWorld();
    /// THE MEASUREMENT'S TWO ARMS (physics.step_cost; never a user setting):
    /// - setSequentialSteps(true): the worlds built after it step on Bullet's sequential scheduler (one thread: the
    ///   single- vs multi-threaded comparison in one process, Bullet's workers keeping their indices). Game thread only.
    /// - setSleepAfterStep(false): Bullet's workers are NOT sent to sleep after each step (Bullet's own 100 us
    ///   spin-before-sleep then runs on past the step, btTaskScheduler.cpp:512 — the cooldown is Bullet's private
    ///   constant; the hint after the step is its one public knob). Default true.
    static void setSequentialSteps(bool sequential);
    static void setSleepAfterStep(bool sleep);
    /// - setIslandBatching(true): Bullet's IN-ISLAND batched solve (btSequentialImpulseConstraintSolverMt's spatial
    ///   grid, for an island of 250+ manifolds) — OFF in the shipped world: its grid assignment asserts on a float
    ///   rounding (btBatchedConstraints.cpp:729, `coordMax == coordMin + 1`: an abort in Debug, and in Release two
    ///   batches of one phase sharing a body — a race). Islands are still solved in parallel (the solver pool).
    static void setIslandBatching(bool on);

	QHash<QString, btCollisionObject*> collisionObjects;
    QHash<QString, btRigidBody*> hashBodies;
    /// The pre-play LOCAL transform of every body and avatar, restored on
    /// Stop bit-exactly. It used to be the global MATRIX, put back through
    /// setGlobalTransform's decomposition — a lossy round trip that left a
    /// node a few ulps from where Play found it after every Simulate stop
    /// (the play path was saved by PlayBack's own exact restore running after
    /// it). Locals are what a parent-independent restore needs: a body is
    /// written in world space during the step and lands back in local space
    /// through the same parent it had.
    struct SavedLocal { iris::Vec3 pos, scale; iris::Quat rot; };
    QHash<QString, SavedLocal> nodeTransforms;

	void addBodyToWorld(btRigidBody *body, const iris::SceneNodePtr &node);
	/// Adds the body AND takes ownership of every allocation behind it (the
	/// collision shape, a compound's children, the triangle-mesh interfaces).
	/// This is the only overload that leaves the world leak-free.
	void addBodyToWorld(PhysicsBody &owned, const iris::SceneNodePtr &node);
	void removeBodyFromWorld(btRigidBody *body);
	void removeBodyFromWorld(const QString &guid);

    void storeCollisionShape(btCollisionShape *shape);
    void storeMeshInterface(btStridingMeshInterface *iface);

    /// Collision shapes / mesh interfaces this world owns. Zero after
    /// destroyPhysicsWorld(); the play/stop gate asserts on both.
    int ownedShapeCount() const { return collisionShapes.size(); }
    int ownedMeshInterfaceCount() const { return meshInterfaces.size(); }

    void addConstraintToWorld(btTypedConstraint *constraint, bool disableCollisions = true);
    void removeConstraintFromWorld(btTypedConstraint *constraint);

	void initializePhysicsWorldFromScene(const iris::SceneNodePtr rootNode);

    btDynamicsWorld *getWorld();

    // These are special functions used for creating a constraint to drag bodies
	void simulatePhysics();
	bool isSimulating();
	void stopPhysics();
	void stopSimulation();
	void stepSimulation(float delta);

	// ---- AVATARS (AVATAR_LOCOMOTION_SPEC §6.3) ---------------------------
	//
	// The seam the 2016 `updateCharacterControllers` call left behind, filled
	// with a component that owns NOTHING in the world. Every registered avatar
	// steps, possessed or not (§8.4: an unpossessed avatar must idle, not
	// freeze), and there is no `break` — the defect that made "which of two
	// characters walks" depend on QHash iteration order is not reproduced.
	void addAvatarToWorld(const iris::SceneNodePtr &node);
	void removeAvatarFromWorld(const QString &guid);
	void removeAllAvatarsFromWorld();
	int avatarCount() const { return avatars.size(); }
	/// Steps every registered avatar's movement component. Called from
	/// stepSimulation AFTER the rigid-body solve, so a sweep sees this frame's
	/// world.
	void updateAvatarMovement(float delta);

	/// Static triangle-mesh colliders for every `collisionEnabled` mesh that is
	/// not already a physics body (§6.3 option C). Built LAZILY — only when the
	/// scene actually contains an avatar — because the spec's own objection to
	/// option B was the seconds-per-Play cost of building a BVH for a whole
	/// level, and a scene with no character has nothing to collide with.
	void buildCollisionContent(const iris::SceneNodePtr &rootNode);

	/// THE HAND HANDS THE BODY BACK (PLAY-SELECT-1). A node moved by the editor
	/// while its simulation runs — the gizmo during play-in-place — is carrying a
	/// pose Bullet knows nothing about: the body is still where the solver last
	/// put it, and the first step after the gesture would teleport the object
	/// back. This writes the NODE's world pose onto its body (and its motion
	/// state), zeroes the velocities and the accumulated forces and wakes it, so
	/// the object carries on from where it was put. False when the node has no
	/// body in this world, which is the ordinary case for a moved prop.
	///
	/// It lives here because the rule "no bullet types outside Environment"
	/// (this header's opening line) is the reason the viewport cannot do it
	/// itself.
	bool syncBodyToNode(const iris::SceneNodePtr &node);

	// ---- THE WRITE-BACK (SPEED-CPU item 1; perf audit 2026-10-03 D1) -------
	//
	// Bullet integrates a body only while its island is awake, so a body that
	// was asleep through every step of a frame is exactly where it was: its
	// node needs no write. The write-back used to write EVERY body EVERY frame,
	// asleep or not, which turned a resting pile into a mover per body per
	// frame — a mirror visit each and the movement epoch moving, so every gated
	// engine walk re-ran over every item. Now:
	//   * the bodies ACTIVE in any step of the frame are written (noted after
	//     each step: a body integrates only in a step it is active in, and
	//     Bullet puts an island to sleep before the integration, never after);
	//   * a sleeping body is RE-CHECKED only when something else in the
	//     document wrote a transform since the last write-back (the epoch,
	//     graph::transformWrites()) — an undo, a reparent, a property track, a
	//     moved parent — and written only when its node no longer holds what the
	//     last write left there (graph::PoseWitness: bit-exact, so a re-check
	//     writes exactly when the old write-everything loop would have changed
	//     something);
	//   * a body NESTED under another body, or on a node whose writes do not
	//     count as scene movement (a camera), is re-checked every frame, after
	//     the others and parents first — the epoch cannot see its parent move.
	// So a sleeping pile's frame makes ZERO writes, and the document it leaves
	// is bit-identical to the one the write-everything loop left
	// (document.physics_writeback).
	void writeBack(const QHash<QString, iris::SceneNodePtr> &nodes);
	/// Nodes the last writeBack wrote (a suite and monitor read).
	int lastWriteBackWrites() const { return lastWrites; }
	/// Sleeping bodies the last writeBack RE-CHECKED (step 2: under the write
	/// journal's nodes, or all of them when the journal was incomplete) — the
	/// suite's count of what a write elsewhere costs.
	int lastWriteBackRechecks() const { return lastRechecks; }

	void restoreNodeTransformations(iris::SceneNodePtr rootNode);
	void restoreNodeTransformationsRecursive(const iris::SceneNodePtr &node);

    void restartPhysics();
    void createPhysicsWorld();
    void destroyPhysicsWorld();

	// These manage a unique picking constraint that is used to manipulate a rigid body about a scene
	// Primarily used in the 3D viewport, the constraint can be loosened to behave more interactively
	void createPickingConstraint(PickingHandleType handleType, const QString &pickedNodeGUID, const btVector3 &hitPoint, const iris::Vec3 &segStart, const iris::Vec3 &segEnd);
	void updatePickingConstraint(PickingHandleType handleType, const btVector3 &rayDirection, const btVector3 &cameraPosition);
	void updatePickingConstraint(PickingHandleType handleType, const iris::Mat4 &handTransformation);
	void cleanupPickingConstraint(PickingHandleType handleType);

	void createConstraintBetweenNodes(iris::SceneNodePtr node, const QString &to, const iris::PhysicsConstraintType &type);
	void setWorldGravity(float gravity);
	float getWorldGravity();

private:
    // createPhysicsWorld() fills all five and destroyPhysicsWorld() empties them,
    // both from the constructor's body — a helper, so these are nullptr of their
    // own until it runs (and after a destroy that returns early).
    btCollisionConfiguration    *collisionConfig = nullptr;
    btDispatcher                *dispatcher = nullptr;
    btBroadphaseInterface       *broadphase = nullptr;
    btConstraintSolver          *solver = nullptr;
    /// THE MULTI-THREADED WORLD'S SOLVER POOL (ENGINE-UPGRADE-U4 stage 6): one solver per task-scheduler thread, the
    /// islands handed out across them (btConstraintSolverPoolMt); null in a build without BT_THREADSAFE.
    /// The one Bullet step (bindGameThread's rules): the world's own stepSimulation, the scheduler's checks around it.
    void                         stepWorld(float delta);
    btConstraintSolver          *solverPool = nullptr;
    /// The scheduler's thread count this world's per-thread arrays were sized for (0 = the single-threaded world): a
    /// step under a scheduler of MORE threads would index past them, so stepSimulation refuses it.
    int                          worldThreads = 0;
    btDynamicsWorld             *world = nullptr;
	
	QHash<int, PickingHandle> pickingHandles;

	/// THE WRITE-BACK'S BOOKKEEPING, per body (see writeBack).
	struct BodyRecord
	{
		QString guid;
		int depth = 0;                 ///< ancestors of the node: parents are written first
		bool nested = false;           ///< a body ancestor, or a node whose writes are not movement
		bool moved = false;            ///< active in one of this frame's steps
		bool pinned = false;           ///< `witness` holds the node as the last write left it
		graph::PoseWitness witness;
	};
	QHash<const btRigidBody *, BodyRecord> bodyRecords;
	QVector<btRigidBody *> movedBodies;     ///< this frame's, de-duplicated by BodyRecord::moved
	QVector<btRigidBody *> nestedBodies;    ///< re-checked every frame, shallowest first
	unsigned long long writesAfterWriteBack = 0;
	bool writeBackValid = false;           ///< false until a write-back ran over every body
	int lastWrites = 0;
	int lastRechecks = 0;
	/// THIS WORLD'S CURSOR ON THE WRITE JOURNAL (graph::WriteJournalReader): the writes since its own last
	/// write-back — taken before step 1, skipped past right after the write-back (its own writes are bodies it
	/// just wrote). Its own cursor, so a mirror's emit draining the same journal never costs it its re-check.
	graph::WriteJournalReader writeJournal;
	void noteActiveBodies();
	void indexBody(btRigidBody *body, const iris::SceneNodePtr &node);
	void unindexBody(const btRigidBody *body);
	void resetWriteBack();

    /// The registered avatars, WEAKLY. A strong reference here would make the
    /// world the thing that keeps a deleted avatar alive, and "delete a moving
    /// avatar mid-play" (gate M7) would then be a leak rather than a removal.
    /// Expired entries are pruned by updateAvatarMovement.
    QVector<iris::SceneNodeWPtr> avatars;
    /// The guids of the nodes `buildCollisionContent` gave a static collider,
    /// so a second call is idempotent.
    QSet<QString> collisionContentNodes;

    QVector<btTypedConstraint*> constraints;
    /// Owned. Destroyed front-to-back at teardown, so a compound shape comes
    /// before its children (PhysicsHelper::createPhysicsBody builds it so).
    btAlignedObjectArray<btCollisionShape*>	collisionShapes;
    /// Owned, and destroyed AFTER collisionShapes: a btConvexTriangleMeshShape
    /// reads its striding interface in its own destructor path.
    QVector<btStridingMeshInterface*> meshInterfaces;
    /// The ghost-pair callback the broadphase's pair cache points at but does
    /// NOT own. One per world; deleted after the broadphase.
    btGhostPairCallback *ghostPairCallback = nullptr;

	btScalar worldYGravity;

    bool simulating;
    bool simulationStarted;

	/*
	btRigidBody *activeRigidBodyBeingManipulated;
	btTypedConstraint *activePickingConstraint;
	int	activeRigidBodySavedState;
	btVector3 constraintOldPickingPosition;
	btVector3 constraintHitPosition;
	btScalar constraintOldPickingDistance;
	*/
};

}

#endif // ENVIRONMENT_H