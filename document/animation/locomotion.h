/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_LOCOMOTION_H
#define IRIS_LOCOMOTION_H

// The locomotion STATE MACHINE — data, not a graph language
// (AVATAR_LOCOMOTION_SPEC §7, Stage 4).
//
// It consumes the movement component's five-parameter contract (§5), which
// AvatarMovement already computes, and produces the one thing the renderer
// needs: a per-frame list of {clip, weight, ABSOLUTE time}. Stage 5 lifts that
// list into `Engine::setClipStates`; nothing here touches the mirror.
//
// ---------------------------------------------------------------------------
// WHY THE STATE LIVES IN THE DOCUMENT (§3.2a, and it is a requirement rather
// than a preference). `SceneMirror::evacuateEngineObjects` releases EVERY entry
// when the other page takes the shared graph, and `releaseEntry` ends with
// `e = Entry();` — so a phase, a blend clock or a "which state am I in" kept
// mirror-side is destroyed on every editor<->player toggle. The document is the
// only place a per-avatar clock survives, which is why this class hangs off
// SceneNode and why the mirror's job is a pure translation.
//
// THE CLOCK RULE, honoured (§13 R1). Times published here are ABSOLUTE seconds
// into each clip, derived from a per-avatar phase this class advances. There is
// deliberately no relative `addTime` anywhere on this path: a relative clock
// makes every pose assertion order-dependent, and two avatars that started
// walking two seconds apart must not be in lock-step.
//
// R2, ENFORCED HERE RATHER THAN IN REVIEW: "no clip enabled" is a FROZEN pose,
// not a bind pose. Every state must resolve to at least one clip — validation
// refuses a state that cannot, and the evaluator never publishes an empty set
// while a state is active.
//
// ---------------------------------------------------------------------------
// THE CONDITION VOCABULARY IS CLOSED. No expressions, no boolean algebra, no
// user-defined parameters, no priorities, no conduits, no nested machines.
//
//     speed > x        speed < x        grounded        !grounded
//     clipEnded(f)     jumpRequested
//
// DEVIATION FROM §7.1, STATED PLAINLY: the spec's §7.1 table lists FIVE forms
// and omits `jumpRequested` — but §7.2's shipped default asset uses
// `jumpRequested` as transition 1's condition ("the latch, read as a
// condition", with a note under the table explaining why it is safe), and §5
// lists it as one of the five contract parameters. The two sections
// contradict each other and there is no reading in which the default asset's
// jump chain works without it: `!grounded` alone routes Grounded -> Fall and
// JumpStart is never entered, which is exactly what gate S3 forbids. So the
// vocabulary here has SIX forms. It is still closed, still flat, still free of
// expressions, and every form still reads one §5 parameter and nothing else.
//
// EVALUATION: once per fixed step, FIRST MATCH WINS in list order. A transition
// already in progress is not re-evaluated until it completes, so a condition
// that flickers cannot restart the same blend every step.
//
// AND ONE SEMANTIC RULE THE SPEC IMPLIES BUT DOES NOT SPELL OUT: a wildcard
// `from` means "any OTHER state" — it does not speak for a state that already
// carries an EXPLICIT transition to the same destination. §7.2's transition 3
// is `* -> Fall` on `!grounded`, and without this rule it would fire on the
// first airborne step of JumpStart (whose own `clipEnded(0.9)` row is still
// false) and cut the jump-start clip to a single frame, silently — which is
// exactly what gate S3 forbids. Unreal draws the same fence around its Any
// State node. See the comment at the evaluation site.

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "irisglfwd.h"

class QJsonObject;

namespace iris
{

struct AvatarLocomotionState;   // document/physics/avatarmovement.h — the §5 contract

// ---------------------------------------------------------------------------
// CONDITIONS

enum class LocomotionConditionKind : int
{
    SpeedGreater = 0,   ///< `speed > x`      — reads §5 speed
    SpeedLess = 1,      ///< `speed < x`      — reads §5 speed
    Grounded = 2,       ///< `grounded`       — reads §5 grounded
    NotGrounded = 3,    ///< `!grounded`      — reads §5 grounded
    ClipEnded = 4,      ///< `clipEnded(f)`   — the state's OWN normalized phase >= f
    JumpRequested = 5   ///< `jumpRequested`  — reads §5 jumpRequested (the latch)
};

struct LocomotionCondition
{
    LocomotionConditionKind kind = LocomotionConditionKind::Grounded;
    /// `x` for the speed forms, the fraction for clipEnded, unused otherwise.
    float value = 0.0f;

    /// The canonical spelling. Round-trips through `parse` byte for byte, which
    /// is what makes the save/load gate (S6) an equality check on strings
    /// rather than a structural walk.
    QString text() const;
    /// Parses one of the six forms. On failure returns false and writes a
    /// message that NAMES the offending text (gate S5) plus the whole
    /// vocabulary — a closed vocabulary is only usable if the refusal says
    /// what the alternatives are.
    static bool parse(const QString &text, LocomotionCondition &out, QString *error);
};

// ---------------------------------------------------------------------------
// SOURCES

/// One sample of a BlendSpace1D (§7.3). `authoredSpeed` is the world speed the
/// clip was AUTHORED to move at, and it is not optional: the play rate of the
/// blend is `speed / sum(w_i * authoredSpeed_i)`, and without it raising
/// runSpeed from 5.5 to 8 makes the feet slide.
struct LocomotionBlendSample
{
    QString clip;
    float position = 0.0f;        ///< the `speed` value this sample sits at
    float authoredSpeed = 0.0f;   ///< 0 = "unmeasured", which disables rate scaling
};

/// The one-dimensional blend space. Its parameter is fixed to `speed` in v1, so
/// it is not stored: there is nothing else it could be.
struct LocomotionBlendSpace
{
    QVector<LocomotionBlendSample> samples;   ///< sorted by position, strictly increasing
    bool syncClips = true;
};

enum class LocomotionSourceKind : int
{
    Clip = 0,
    BlendSpace = 1
};

struct LocomotionStateDef
{
    QString name;
    LocomotionSourceKind kind = LocomotionSourceKind::Clip;
    QString clip;                 ///< kind == Clip
    float speed = 1.0f;           ///< kind == Clip: play-rate multiplier
    LocomotionBlendSpace space;   ///< kind == BlendSpace
    bool loop = true;
};

struct LocomotionTransitionDef
{
    QString from;                 ///< a state name, or "*" for any state
    QString to;
    LocomotionCondition condition;
    float blendDuration = 0.15f;  ///< seconds; 0 is an instant cut
};

/// The asset. `transitions` ORDER IS THE PRIORITY — there is no priority field
/// and there never will be one (§7.1).
struct LocomotionAsset
{
    QString name;
    QVector<LocomotionStateDef> states;
    QVector<LocomotionTransitionDef> transitions;
    QString entry;

    bool isEmpty() const { return states.isEmpty(); }
    const LocomotionStateDef *findState(const QString &name) const;
    /// Structural validation (§10 `setLocomotionAsset`): every from/to names a
    /// state (or "*" for from), the entry exists, every state resolves to at
    /// least one clip (R2), a blend space has >= 2 samples with STRICTLY
    /// increasing positions, no blend duration is negative. The condition
    /// vocabulary is checked at PARSE time, not here — a LocomotionCondition
    /// that exists is by construction one of the six forms.
    bool validate(QString *error) const;
};

// ---------------------------------------------------------------------------
// CLIP ROLES (§7.4)

enum class ClipRole : int
{
    Idle = 0,
    Walk = 1,
    Run = 2,
    JumpStart = 3,
    FallLoop = 4,
    Land = 5
};
static constexpr int kClipRoleCount = 6;

/// `idle`, `walk`, `run`, `jump-start`, `fall-loop`, `land`.
const char *clipRoleName(ClipRole role);
/// Accepts the canonical spelling and the camelCase one the verbs report
/// (`jumpStart`, `fallLoop`), case-insensitively.
bool clipRoleFromName(const QString &name, ClipRole &out);

/// role -> clip name. An UNBOUND role is an empty string, and that is not an
/// error: §7.4 — "a missing role is not an error", the default asset degrades.
struct ClipRoles
{
    QString names[kClipRoleCount];

    bool has(ClipRole r) const { return !names[int(r)].isEmpty(); }
    const QString &get(ClipRole r) const { return names[int(r)]; }
    void set(ClipRole r, const QString &clip) { names[int(r)] = clip; }
    int boundCount() const;
};

/// TOLERANT NAME MATCHING against the Mixamo-junk-name reality (§7.4). Every
/// Mixamo character download ships a single-frame clip literally called
/// "mixamo.com"; every Mixamo ANIMATION download is named for the animation
/// ("Walking", "Slow Run", "Jumping Up", "Falling Idle", "Falling To Landing")
/// but only after the importer has substituted the file's base name for the
/// junk one. So the matcher works on words, not on equality, and it resolves
/// roles in an order that settles the overlaps rather than leaving them to
/// list order:
///
///   land -> jump-start -> fall-loop -> run -> walk -> idle
///
/// "Falling To Landing" contains both `fall` and `land` and must be LAND;
/// "Falling Idle" contains both `fall` and `idle` and must be FALL-LOOP. Each
/// clip binds to at most one role, and a role takes the best-scoring unbound
/// candidate (exact word match beats a substring; ties break on list order, so
/// the result is deterministic for a given clip list).
ClipRoles matchClipRoles(const QStringList &clipNames);

// ---------------------------------------------------------------------------
// THE DEFAULT ASSET (§7.2)

/// "Biped Locomotion", built from whatever roles bound, exactly as §7.2's two
/// tables specify — and DEGRADED where a role is missing (§7.4):
///
///   * no `run`      -> the Grounded blend space is two samples (idle, walk)
///   * one clip only -> Grounded becomes a plain clip state, not a 1-sample space
///   * no `land`     -> the Land state and transition 5 are dropped and
///                      transition 4 targets Grounded directly
///   * no `jump-start` -> the JumpStart state and transition 1 are dropped;
///                      the wildcard `* -> Fall` on !grounded still catches the
///                      jump, so a jump still leaves the ground looking right
///   * no `fall-loop`  -> the Fall state is dropped with every transition that
///                      names it, and JumpStart returns to Grounded on
///                      clipEnded(0.9)
///   * nothing bound -> an EMPTY asset. The evaluator publishes no weights and
///                      says so; it never publishes an empty state set, which
///                      would freeze the pose (R2).
///
/// `walkSpeed`/`runSpeed` are the movement component's, so the blend space's
/// sample positions track the knobs the user actually set.
LocomotionAsset buildDefaultLocomotionAsset(const ClipRoles &roles, float walkSpeed,
                                            float runSpeed);
/// The shipped name, in one place: "Biped Locomotion".
const char *defaultLocomotionAssetName();

// ---------------------------------------------------------------------------
// SERIALIZATION — ONE grammar, shared by `src/io` and by the verbs, so a scene
// file and `avatar.setLocomotionAsset` cannot drift apart. Conditions are
// stored as their CANONICAL TEXT rather than as an enum int: a file that stored
// an int would freeze today's enum order (the rule giMode and playMode are
// written under) and a human reading the scene JSON would learn nothing.

QJsonObject locomotionAssetToJson(const LocomotionAsset &asset);
/// Parses AND validates. On refusal `out` is untouched and `*error` names the
/// offending transition and condition text (gate S5).
bool locomotionAssetFromJson(const QJsonObject &obj, LocomotionAsset &out, QString *error);

/// clip name -> length in seconds, for one avatar wrapper. The FIRST node in
/// the subtree (the wrapper included, depth-first) carrying skeletal animations
/// is the clip host — `SceneMirror::clipHostOf`'s rule read from the other end.
void collectAvatarClips(const SceneNodePtr &node, QMap<QString, float> &out);

// ---------------------------------------------------------------------------
// THE EVALUATOR'S OUTPUT

/// One entry of the per-frame push. `time` is ABSOLUTE seconds into `clip`.
struct LocomotionClipWeight
{
    QString clip;
    float weight = 0.0f;
    float time = 0.0f;
    bool looping = true;
};

/// The per-avatar state machine instance. Deterministic under a fixed dt by
/// construction: no wall clock, no hash iteration, no pointer ordering — the
/// same dt sequence produces bit-identical output (gate S7).
class AvatarLocomotion
{
public:
    AvatarLocomotion();

    AvatarLocomotion(const AvatarLocomotion &) = delete;
    AvatarLocomotion &operator=(const AvatarLocomotion &) = delete;

    // ---- the asset --------------------------------------------------------

    const LocomotionAsset &asset() const { return mAsset; }
    /// Validates and installs. On refusal nothing changes and `*error` names
    /// what was wrong (gate S5). Resets the runtime to the entry state.
    bool setAsset(const LocomotionAsset &asset, QString *error);
    /// The same, WITHOUT clearing the "this is the generated default" flag —
    /// the scene reader and the node-duplication path both restore an asset
    /// that may legitimately still be the default one, and marking it authored
    /// would stop it ever tracking a later role bind or knob change.
    bool setAssetPreservingDefaultFlag(const LocomotionAsset &asset, QString *error);
    /// Restores an asset AND the flag together — what an undo needs, since
    /// undoing an authored asset back to the generated default has to restore
    /// "this is the default" too, or the next role bind would refuse to
    /// regenerate it.
    bool restoreAsset(const LocomotionAsset &asset, bool isDefault, QString *error);
    /// True while the asset is the generated default — the flag that lets a
    /// role rebind or a knob change REGENERATE it, without ever silently
    /// overwriting an asset the user authored.
    bool usesDefaultAsset() const { return mIsDefault; }

    // ---- roles ------------------------------------------------------------

    const ClipRoles &roles() const { return mRoles; }
    /// Binds one role by hand. Regenerates the default asset when that is what
    /// is installed. An empty `clip` UNBINDS the role, which is how a user
    /// tells the default asset to skip a state.
    void setClipRole(ClipRole role, const QString &clip);
    /// Runs the tolerant matcher over `clipNames` and installs the default
    /// asset built from the result. This is what spawn and load call.
    void bindRolesFromClips(const QStringList &clipNames, float walkSpeed, float runSpeed);
    /// Rebuilds the default asset from the CURRENT roles (after a knob change).
    /// A no-op when the installed asset is not the default, and it KEEPS the
    /// running state rather than throwing a walking character back to entry.
    void refreshDefaultAsset(float walkSpeed, float runSpeed);
    /// Installs the generated default asset for `roles`. What spawn calls.
    void setDefaultAsset(const ClipRoles &roles, float walkSpeed, float runSpeed);
    /// What the scene READER calls: the roles a file recorded are treated as
    /// MANUAL (they were resolved once and saved, so a later auto-match must
    /// not clobber them), and `isDefaultAsset` says whether the asset that
    /// follows is the generated one or an authored one.
    void markRolesFromFile(const ClipRoles &roles, bool isDefaultAsset);

    // ---- the step ---------------------------------------------------------

    /// One fixed step. `params` is the §5 contract as the movement component
    /// published it THIS step — including `jumpRequested`, which is true only
    /// in the step the component consumed the latch, which is why the machine
    /// must be stepped immediately after the movement and not a frame later.
    ///
    /// `clipLengths` maps clip name -> seconds. A clip the map does not know
    /// is treated as zero-length: it is still published (the engine pads a
    /// zero-length clip rather than refusing it) but it is excluded from the
    /// blended cycle length and from phase advance, exactly as §7.3 requires.
    void step(const AvatarLocomotionState &params, const QMap<QString, float> &clipLengths,
              float dt);
    /// The same step, with the clip table refreshed from the node's own
    /// animations first — what the physics tick calls, so a clip loaded onto an
    /// already-spawned character re-matches its roles without anyone asking.
    void stepForNode(const SceneNodePtr &node, const AvatarLocomotionState &params, float dt,
                     float walkSpeed, float runSpeed);
    /// Rebuilds the cached clip table from `node` IF it changed. Public because
    /// the verbs report clip lengths and must not report a stale table.
    void refreshClips(const SceneNodePtr &node, float walkSpeed, float runSpeed);
    const QMap<QString, float> &clipLengths() const { return mClipLengths; }
    /// Back to the entry state with a zero phase and no transition in flight.
    void reset();

    // ---- what it did ------------------------------------------------------

    /// The state the machine is IN. During a transition this is already the
    /// DESTINATION — that is the Unreal/Unity reading and it is what makes
    /// "the sequence of states a speed ramp produced" a well-defined list.
    const QString &currentState() const { return mCurrent; }
    /// The state being blended OUT of, empty when no transition is in flight.
    const QString &previousState() const { return mPrevious; }
    bool isTransitioning() const { return mTransitioning; }
    /// `u` in [0,1]: 0 at the instant the transition was taken, 1 the step it
    /// completes. 1 when nothing is in flight.
    float transitionProgress() const { return mTransitioning ? mBlendU : 1.0f; }
    float transitionDuration() const { return mBlendDuration; }
    /// The normalized phase of the current state, 0..1.
    float statePhase() const { return mPhase; }

    /// The per-frame push: {clip, weight, absolute time}. Never empty while a
    /// state is active (R2). Ordered: the destination state's clips first, in
    /// blend-sample order, then the outgoing state's — a stable order, so two
    /// identical runs produce identical arrays.
    const QVector<LocomotionClipWeight> &weights() const { return mWeights; }

private:
    struct Sample
    {
        QString clip;
        float weight = 0.0f;
        float length = 0.0f;
        float authoredSpeed = 0.0f;
    };

    /// The clips a state contributes at `speed`, with their weights (summing to
    /// 1 before the cross-fade scales them).
    void sampleState(const LocomotionStateDef &st, float speed,
                     const QMap<QString, float> &clipLengths, QVector<Sample> &out) const;
    /// Advances one state's phase by `dt` and returns the new phase.
    static float advancePhase(const LocomotionStateDef &st, const QVector<Sample> &samples,
                              float phase, float speed, float dt);
    void emitWeights(const LocomotionStateDef &st, const QVector<Sample> &samples, float phase,
                     float scale);
    bool conditionHolds(const LocomotionCondition &c, const AvatarLocomotionState &params,
                        float phase) const;
    /// True when `from` carries a non-wildcard transition to `to` — the test
    /// behind the "a wildcard means any OTHER state" rule at the top of this
    /// file.
    bool hasExplicitTransition(const QString &from, const QString &to) const;

    LocomotionAsset mAsset;
    ClipRoles mRoles;
    /// Per role: was this binding a HUMAN's (or a file's), rather than the
    /// matcher's? A manual binding survives every later auto-match.
    bool mRoleIsManual[kClipRoleCount] = { false, false, false, false, false, false };
    bool mIsDefault = true;
    float mDefaultWalkSpeed = 2.0f;
    float mDefaultRunSpeed = 5.5f;
    QMap<QString, float> mClipLengths;

    QString mCurrent;
    QString mPrevious;
    float mPhase = 0.0f;        ///< the CURRENT state's normalized phase
    float mPrevPhase = 0.0f;    ///< the outgoing state's, while blending

    bool  mTransitioning = false;
    float mBlendElapsed = 0.0f;
    float mBlendDuration = 0.0f;
    float mBlendU = 1.0f;

    QVector<LocomotionClipWeight> mWeights;
};

}   // namespace iris

#endif   // IRIS_LOCOMOTION_H
