/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/animation/locomotion.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

#include "document/animation/animation.h"
#include "document/physics/avatarmovement.h"
#include "document/scenegraph/scenenode.h"

namespace iris
{

namespace
{
/// The float spelling used in every canonical condition string. 'g' with 6
/// significant digits round-trips a float exactly for every value a human
/// types and never grows an exponent for the range conditions live in.
QString num(float v) { return QString::number(double(v), 'g', 6); }

/// The whole vocabulary, in the refusal message. A closed vocabulary is only
/// usable if being refused tells you what the alternatives are (gate S5).
const char *kVocabulary =
    "speed > x, speed < x, grounded, !grounded, clipEnded(fraction), jumpRequested";
}   // namespace

// ---------------------------------------------------------------------------
// CONDITIONS

QString LocomotionCondition::text() const
{
    switch (kind) {
    case LocomotionConditionKind::SpeedGreater: return QStringLiteral("speed > ") + num(value);
    case LocomotionConditionKind::SpeedLess:    return QStringLiteral("speed < ") + num(value);
    case LocomotionConditionKind::Grounded:     return QStringLiteral("grounded");
    case LocomotionConditionKind::NotGrounded:  return QStringLiteral("!grounded");
    case LocomotionConditionKind::ClipEnded:
        return QStringLiteral("clipEnded(") + num(value) + QLatin1Char(')');
    case LocomotionConditionKind::JumpRequested: return QStringLiteral("jumpRequested");
    }
    return QString();
}

bool LocomotionCondition::parse(const QString &text, LocomotionCondition &out, QString *error)
{
    auto refuse = [&](const QString &why) {
        if (error)
            *error = QStringLiteral("unrecognised condition \"%1\": %2 — the condition "
                                    "vocabulary is CLOSED, and the six forms are: %3")
                         .arg(text.trimmed(), why, QLatin1String(kVocabulary));
        return false;
    };

    // Whitespace is not significant anywhere in the grammar, so it is removed
    // once rather than handled per form. Case is not significant either: a user
    // who typed `ClipEnded` meant clipEnded.
    QString c = text;
    c.remove(QRegularExpression(QStringLiteral("\\s")));
    if (c.isEmpty()) return refuse(QStringLiteral("it is empty"));
    const QString lower = c.toLower();

    if (lower == QLatin1String("grounded")) {
        out.kind = LocomotionConditionKind::Grounded;
        out.value = 0.0f;
        return true;
    }
    if (lower == QLatin1String("!grounded")) {
        out.kind = LocomotionConditionKind::NotGrounded;
        out.value = 0.0f;
        return true;
    }
    if (lower == QLatin1String("jumprequested")) {
        out.kind = LocomotionConditionKind::JumpRequested;
        out.value = 0.0f;
        return true;
    }
    if (lower.startsWith(QLatin1String("speed>")) || lower.startsWith(QLatin1String("speed<"))) {
        const bool greater = lower.at(5) == QLatin1Char('>');
        bool ok = false;
        const float v = lower.mid(6).toFloat(&ok);
        if (!ok || !std::isfinite(v))
            return refuse(QStringLiteral("'%1' is not a number").arg(c.mid(6)));
        out.kind = greater ? LocomotionConditionKind::SpeedGreater
                           : LocomotionConditionKind::SpeedLess;
        out.value = v;
        return true;
    }
    if (lower.startsWith(QLatin1String("clipended("))) {
        if (!lower.endsWith(QLatin1Char(')')))
            return refuse(QStringLiteral("the closing parenthesis is missing"));
        const QString inner = lower.mid(10, lower.size() - 11);
        bool ok = false;
        const float v = inner.toFloat(&ok);
        if (!ok || !std::isfinite(v))
            return refuse(QStringLiteral("'%1' is not a number").arg(inner));
        // A fraction OF THE CLIP. Outside [0,1] it either fires on the first
        // step or never fires at all, and both read as a broken state machine.
        if (v < 0.0f || v > 1.0f)
            return refuse(QStringLiteral("the fraction %1 is outside [0, 1]").arg(num(v)));
        out.kind = LocomotionConditionKind::ClipEnded;
        out.value = v;
        return true;
    }
    // `speed >= x`, `speed == x`, `!jumpRequested`, `speed > x && grounded` and
    // every other near-miss lands here, by design: the vocabulary is the fence.
    return refuse(QStringLiteral("no form of the vocabulary matches it"));
}

// ---------------------------------------------------------------------------
// THE ASSET

const LocomotionStateDef *LocomotionAsset::findState(const QString &n) const
{
    for (const auto &s : states)
        if (s.name == n) return &s;
    return nullptr;
}

bool LocomotionAsset::validate(QString *error) const
{
    auto fail = [&](const QString &msg) {
        if (error) *error = msg;
        return false;
    };

    if (states.isEmpty()) return fail(QStringLiteral("the asset has no states"));

    for (int i = 0; i < states.size(); ++i) {
        const LocomotionStateDef &s = states[i];
        if (s.name.isEmpty())
            return fail(QStringLiteral("state %1 has no name").arg(i));
        if (s.name == QLatin1String("*"))
            return fail(QStringLiteral("state %1 is named '*', which is the wildcard "
                                       "a transition's `from` uses").arg(i));
        for (int j = 0; j < i; ++j)
            if (states[j].name == s.name)
                return fail(QStringLiteral("two states are named '%1'").arg(s.name));

        // R2: "no clip enabled" is a FROZEN pose, not a bind pose. A state that
        // cannot resolve to at least one clip produces a character stuck in a
        // half-pose with no error anywhere, so it is refused here rather than
        // discovered in a demo.
        if (s.kind == LocomotionSourceKind::Clip) {
            if (s.clip.isEmpty())
                return fail(QStringLiteral("state '%1' names no clip").arg(s.name));
            if (!(s.speed > 0.0f))
                return fail(QStringLiteral("state '%1' has a play speed of %2; it must be "
                                           "positive").arg(s.name, num(s.speed)));
        } else {
            const auto &sm = s.space.samples;
            if (sm.size() < 2)
                return fail(QStringLiteral("state '%1' is a blend space with %2 sample(s); "
                                           "a blend space needs at least 2")
                                .arg(s.name).arg(sm.size()));
            for (int k = 0; k < sm.size(); ++k) {
                if (sm[k].clip.isEmpty())
                    return fail(QStringLiteral("state '%1' blend sample %2 names no clip")
                                    .arg(s.name).arg(k));
                if (k > 0 && !(sm[k].position > sm[k - 1].position))
                    return fail(QStringLiteral("state '%1' blend sample positions must be "
                                               "STRICTLY increasing: sample %2 sits at %3, "
                                               "sample %4 at %5")
                                    .arg(s.name).arg(k - 1).arg(num(sm[k - 1].position))
                                    .arg(k).arg(num(sm[k].position)));
                if (sm[k].authoredSpeed < 0.0f)
                    return fail(QStringLiteral("state '%1' blend sample %2 has a negative "
                                               "authoredSpeed").arg(s.name).arg(k));
            }
        }
    }

    if (entry.isEmpty()) return fail(QStringLiteral("the asset names no entry state"));
    if (!findState(entry))
        return fail(QStringLiteral("the entry state '%1' is not one of the asset's states")
                        .arg(entry));

    for (int i = 0; i < transitions.size(); ++i) {
        const LocomotionTransitionDef &t = transitions[i];
        if (t.from.isEmpty())
            return fail(QStringLiteral("transition %1 has no 'from'").arg(i));
        if (t.from != QLatin1String("*") && !findState(t.from))
            return fail(QStringLiteral("transition %1: 'from' names '%2', which is not a "
                                       "state (use '*' for any state)").arg(i).arg(t.from));
        if (t.to.isEmpty())
            return fail(QStringLiteral("transition %1 has no 'to'").arg(i));
        if (!findState(t.to))
            return fail(QStringLiteral("transition %1: 'to' names '%2', which is not a state")
                            .arg(i).arg(t.to));
        if (t.from == t.to)
            return fail(QStringLiteral("transition %1 goes from '%2' to itself").arg(i).arg(t.from));
        if (t.blendDuration < 0.0f || !std::isfinite(t.blendDuration))
            return fail(QStringLiteral("transition %1 (%2 -> %3) has a blend duration of %4")
                            .arg(i).arg(t.from, t.to, num(t.blendDuration)));
    }
    return true;
}

// ---------------------------------------------------------------------------
// CLIP ROLES

const char *clipRoleName(ClipRole role)
{
    switch (role) {
    case ClipRole::Idle:      return "idle";
    case ClipRole::Walk:      return "walk";
    case ClipRole::Run:       return "run";
    case ClipRole::JumpStart: return "jump-start";
    case ClipRole::FallLoop:  return "fall-loop";
    case ClipRole::Land:      return "land";
    }
    return "idle";
}

bool clipRoleFromName(const QString &name, ClipRole &out)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("idle")) { out = ClipRole::Idle; return true; }
    if (n == QLatin1String("walk")) { out = ClipRole::Walk; return true; }
    if (n == QLatin1String("run"))  { out = ClipRole::Run; return true; }
    if (n == QLatin1String("jump-start") || n == QLatin1String("jumpstart")) {
        out = ClipRole::JumpStart; return true;
    }
    if (n == QLatin1String("fall-loop") || n == QLatin1String("fallloop")) {
        out = ClipRole::FallLoop; return true;
    }
    if (n == QLatin1String("land")) { out = ClipRole::Land; return true; }
    return false;
}

int ClipRoles::boundCount() const
{
    int n = 0;
    for (int i = 0; i < kClipRoleCount; ++i)
        if (!names[i].isEmpty()) ++n;
    return n;
}

namespace
{
struct RolePatterns
{
    ClipRole role;
    const char *words[8];
};

/// RESOLUTION ORDER, and it is the whole trick (§7.4): `land` before
/// `fall-loop` so "Falling To Landing" is a landing, and `fall-loop` before
/// `idle` so "Falling Idle" is a fall. Mixamo's own clip names are the
/// worked examples throughout.
const RolePatterns kPatterns[] = {
    { ClipRole::Land,      { "land", "landing", "landed", nullptr } },
    { ClipRole::JumpStart, { "jump", "jumping", "jumps", "leap", "leaping", nullptr } },
    { ClipRole::FallLoop,  { "fall", "falling", "falls", "airborne", "midair", nullptr } },
    { ClipRole::Run,       { "run", "running", "runs", "sprint", "sprinting", "jog",
                             "jogging", nullptr } },
    { ClipRole::Walk,      { "walk", "walking", "walks", "stride", "striding", nullptr } },
    { ClipRole::Idle,      { "idle", "idling", "breathing", "stand", "standing", nullptr } },
};

/// lowercase, punctuation -> spaces. "Falling To Landing" -> "falling to landing",
/// "mixamo.com" -> "mixamo com", "fall_loop" -> "fall loop".
QString normalizeClipName(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (QChar ch : name) {
        if (ch.isLetterOrNumber()) out.append(ch.toLower());
        else out.append(QLatin1Char(' '));
    }
    return out.simplified();
}

/// 3 = the whole name IS the keyword, 2 = a word of it is, 1 = it contains it,
/// 0 = no match. Higher is a better binding; ties break on list order, so the
/// matcher is deterministic for a given clip list (gate S7's sibling property).
int scoreAgainst(const QString &normalized, const QStringList &words, const RolePatterns &p)
{
    int best = 0;
    for (int i = 0; p.words[i]; ++i) {
        const QLatin1String key(p.words[i]);
        if (normalized == key) return 3;
        if (words.contains(QString(key))) best = std::max(best, 2);
        else if (normalized.contains(key)) best = std::max(best, 1);
    }
    return best;
}
}   // namespace

ClipRoles matchClipRoles(const QStringList &clipNames)
{
    ClipRoles roles;
    const int n = clipNames.size();
    QVector<QString> normalized(n);
    QVector<QStringList> words(n);
    QVector<bool> used(n, false);
    for (int i = 0; i < n; ++i) {
        normalized[i] = normalizeClipName(clipNames[i]);
        words[i] = normalized[i].split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (normalized[i].isEmpty()) used[i] = true;   // an unnamed clip binds to nothing
    }

    for (const RolePatterns &p : kPatterns) {
        int bestIdx = -1;
        int bestScore = 0;
        for (int i = 0; i < n; ++i) {
            if (used[i]) continue;
            const int s = scoreAgainst(normalized[i], words[i], p);
            if (s > bestScore) { bestScore = s; bestIdx = i; }
        }
        if (bestIdx >= 0) {
            roles.set(p.role, clipNames[bestIdx]);
            used[bestIdx] = true;
        }
    }
    return roles;
}

// ---------------------------------------------------------------------------
// THE DEFAULT ASSET

const char *defaultLocomotionAssetName() { return "Biped Locomotion"; }

LocomotionAsset buildDefaultLocomotionAsset(const ClipRoles &roles, float walkSpeed,
                                            float runSpeed)
{
    LocomotionAsset a;
    a.name = QLatin1String(defaultLocomotionAssetName());

    // ---- the Grounded blend space (§7.2 row 1) ----------------------------
    QVector<LocomotionBlendSample> samples;
    auto addSample = [&](ClipRole r, float position, float authored) {
        if (!roles.has(r)) return;
        LocomotionBlendSample s;
        s.clip = roles.get(r);
        // STRICTLY increasing, enforced here rather than hoped for: a user who
        // sets walkSpeed to 0 (or runSpeed below walkSpeed) would otherwise
        // build an asset its own validator refuses.
        s.position = samples.isEmpty() ? std::max(0.0f, position)
                                       : std::max(position, samples.last().position + 0.01f);
        s.authoredSpeed = authored;
        samples.append(s);
    };
    // `authoredSpeed` DEFAULTS to the knob the sample sits at — i.e. "this clip
    // was authored to move at exactly the speed I placed it at", which makes
    // the play rate 1.0 at the sample and scales it honestly in between. Real
    // measured values (§7.5 acceptance item 4) replace these per character;
    // idle is 0 because a stationary clip has no authored speed and must not
    // drag the weighted sum toward zero rate.
    addSample(ClipRole::Idle, 0.0f, 0.0f);
    addSample(ClipRole::Walk, walkSpeed, walkSpeed);
    addSample(ClipRole::Run, runSpeed, runSpeed);

    const bool hasGrounded = !samples.isEmpty();
    if (hasGrounded) {
        LocomotionStateDef grounded;
        grounded.name = QStringLiteral("Grounded");
        grounded.loop = true;
        if (samples.size() >= 2) {
            grounded.kind = LocomotionSourceKind::BlendSpace;
            grounded.space.samples = samples;
            grounded.space.syncClips = true;
        } else {
            // ONE clip is not a blend space (validation refuses a 1-sample
            // space, correctly — there is nothing to blend). A character with
            // only an idle clip idles; that is the degradation, not an error.
            grounded.kind = LocomotionSourceKind::Clip;
            grounded.clip = samples.first().clip;
        }
        a.states.append(grounded);
    }

    auto addClipState = [&](const char *name, ClipRole role, bool loop) {
        if (!roles.has(role)) return;
        LocomotionStateDef s;
        s.name = QLatin1String(name);
        s.kind = LocomotionSourceKind::Clip;
        s.clip = roles.get(role);
        s.loop = loop;
        a.states.append(s);
    };
    addClipState("JumpStart", ClipRole::JumpStart, false);
    addClipState("Fall", ClipRole::FallLoop, true);
    addClipState("Land", ClipRole::Land, false);

    if (a.states.isEmpty()) return a;   // nothing bound: an EMPTY asset, not a broken one
    a.entry = a.states.first().name;

    const bool hasJump = a.findState(QStringLiteral("JumpStart")) != nullptr;
    const bool hasFall = a.findState(QStringLiteral("Fall")) != nullptr;
    const bool hasLand = a.findState(QStringLiteral("Land")) != nullptr;

    auto cond = [](const char *text) {
        LocomotionCondition c;
        QString err;
        LocomotionCondition::parse(QLatin1String(text), c, &err);   // built-in: always parses
        return c;
    };
    auto addTransition = [&](const QString &from, const QString &to, const char *condition,
                             float blend) {
        if (from != QLatin1String("*") && !a.findState(from)) return;
        if (!a.findState(to)) return;
        LocomotionTransitionDef t;
        t.from = from;
        t.to = to;
        t.condition = cond(condition);
        t.blendDuration = blend;
        a.transitions.append(t);
    };

    // §7.2's five transitions, IN ORDER — order is the priority, and the first
    // row is why a jump plays JumpStart rather than falling straight to Fall:
    // the movement component clears `grounded` in the same step it applies the
    // impulse, so both row 1 and row 3 hold on that step and row 1 wins.
    addTransition(QStringLiteral("Grounded"), QStringLiteral("JumpStart"), "jumpRequested", 0.05f);
    if (hasFall) {
        addTransition(QStringLiteral("JumpStart"), QStringLiteral("Fall"), "clipEnded(0.9)", 0.10f);
        addTransition(QStringLiteral("*"), QStringLiteral("Fall"), "!grounded", 0.15f);
        if (hasLand) {
            addTransition(QStringLiteral("Fall"), QStringLiteral("Land"), "grounded", 0.10f);
        } else {
            // §7.4: "No `land` -> transition 4 targets Grounded directly."
            addTransition(QStringLiteral("Fall"), QStringLiteral("Grounded"), "grounded", 0.10f);
        }
    } else if (hasJump) {
        // No fall loop: the jump chain has to come back on its own, and
        // clipEnded is exactly the tool that means no user authors a timer.
        addTransition(QStringLiteral("JumpStart"),
                      hasLand ? QStringLiteral("Land") : QStringLiteral("Grounded"),
                      "clipEnded(0.9)", 0.10f);
    }
    addTransition(QStringLiteral("Land"), QStringLiteral("Grounded"), "clipEnded(0.8)", 0.15f);
    return a;
}

// ---------------------------------------------------------------------------
// SERIALIZATION — ONE grammar, used by src/io AND by the verbs, so a scene file
// and `avatar.setLocomotionAsset` can never drift apart (gate S6 covers both
// paths because they are the same code).

QJsonObject locomotionAssetToJson(const LocomotionAsset &asset)
{
    QJsonObject out;
    out[QStringLiteral("name")] = asset.name;
    out[QStringLiteral("entry")] = asset.entry;

    QJsonArray states;
    for (const auto &s : asset.states) {
        QJsonObject o;
        o[QStringLiteral("name")] = s.name;
        o[QStringLiteral("loop")] = s.loop;
        QJsonObject src;
        if (s.kind == LocomotionSourceKind::Clip) {
            src[QStringLiteral("kind")] = QStringLiteral("clip");
            src[QStringLiteral("clip")] = s.clip;
            src[QStringLiteral("speed")] = double(s.speed);
        } else {
            src[QStringLiteral("kind")] = QStringLiteral("blendspace");
            src[QStringLiteral("syncClips")] = s.space.syncClips;
            QJsonArray samples;
            for (const auto &sm : s.space.samples) {
                QJsonObject so;
                so[QStringLiteral("clip")] = sm.clip;
                so[QStringLiteral("position")] = double(sm.position);
                so[QStringLiteral("authoredSpeed")] = double(sm.authoredSpeed);
                samples.append(so);
            }
            src[QStringLiteral("samples")] = samples;
        }
        o[QStringLiteral("source")] = src;
        states.append(o);
    }
    out[QStringLiteral("states")] = states;

    QJsonArray transitions;
    for (const auto &t : asset.transitions) {
        QJsonObject o;
        o[QStringLiteral("from")] = t.from;
        o[QStringLiteral("to")] = t.to;
        // The CANONICAL TEXT, not the enum: a file that stored an int would
        // freeze today's enum order, and a human reading the scene JSON would
        // learn nothing. `text()` round-trips through `parse` byte for byte.
        o[QStringLiteral("condition")] = t.condition.text();
        o[QStringLiteral("blendDuration")] = double(t.blendDuration);
        transitions.append(o);
    }
    out[QStringLiteral("transitions")] = transitions;
    return out;
}

bool locomotionAssetFromJson(const QJsonObject &obj, LocomotionAsset &out, QString *error)
{
    auto fail = [&](const QString &msg) {
        if (error) *error = msg;
        return false;
    };

    LocomotionAsset a;
    a.name = obj.value(QStringLiteral("name")).toString(
        QLatin1String(defaultLocomotionAssetName()));
    a.entry = obj.value(QStringLiteral("entry")).toString();

    const QJsonArray states = obj.value(QStringLiteral("states")).toArray();
    for (int i = 0; i < states.size(); ++i) {
        const QJsonObject o = states.at(i).toObject();
        LocomotionStateDef s;
        s.name = o.value(QStringLiteral("name")).toString();
        s.loop = o.value(QStringLiteral("loop")).toBool(true);
        const QJsonObject src = o.value(QStringLiteral("source")).toObject();
        const QString kind = src.value(QStringLiteral("kind")).toString().toLower();
        if (kind == QLatin1String("blendspace")) {
            s.kind = LocomotionSourceKind::BlendSpace;
            s.space.syncClips = src.value(QStringLiteral("syncClips")).toBool(true);
            const QJsonArray samples = src.value(QStringLiteral("samples")).toArray();
            for (const auto &sv : samples) {
                const QJsonObject so = sv.toObject();
                LocomotionBlendSample sm;
                sm.clip = so.value(QStringLiteral("clip")).toString();
                sm.position = float(so.value(QStringLiteral("position")).toDouble());
                sm.authoredSpeed = float(so.value(QStringLiteral("authoredSpeed")).toDouble());
                s.space.samples.append(sm);
            }
        } else if (kind == QLatin1String("clip") || kind.isEmpty()) {
            s.kind = LocomotionSourceKind::Clip;
            s.clip = src.value(QStringLiteral("clip")).toString();
            s.speed = float(src.value(QStringLiteral("speed")).toDouble(1.0));
        } else {
            return fail(QStringLiteral("state '%1': unknown source kind '%2' (known: clip, "
                                       "blendspace)").arg(s.name, kind));
        }
        a.states.append(s);
    }

    const QJsonArray transitions = obj.value(QStringLiteral("transitions")).toArray();
    for (int i = 0; i < transitions.size(); ++i) {
        const QJsonObject o = transitions.at(i).toObject();
        LocomotionTransitionDef t;
        t.from = o.value(QStringLiteral("from")).toString();
        t.to = o.value(QStringLiteral("to")).toString();
        t.blendDuration = float(o.value(QStringLiteral("blendDuration")).toDouble(0.15));
        QString err;
        if (!LocomotionCondition::parse(o.value(QStringLiteral("condition")).toString(),
                                        t.condition, &err))
            return fail(QStringLiteral("transition %1 (%2 -> %3): %4")
                            .arg(i).arg(t.from, t.to, err));
        a.transitions.append(t);
    }

    if (!a.validate(error)) return false;
    out = a;
    return true;
}

// ---------------------------------------------------------------------------
// CLIP TABLE

void collectAvatarClips(const SceneNodePtr &node, QMap<QString, float> &out)
{
    out.clear();
    if (!node) return;
    // The FIRST node in the subtree (the wrapper included, depth-first) that
    // carries skeletal animations is the clip host. That is
    // SceneMirror::clipHostOf's rule read from the other end: it walks UP from
    // the skinned mesh to find the node holding the clips, and an avatar
    // wrapper is by construction an ancestor of exactly one rigged mesh.
    std::function<const SceneNode *(const SceneNode *)> findHost =
        [&](const SceneNode *n) -> const SceneNode * {
        if (!n) return nullptr;
        for (const auto &anim : const_cast<SceneNode *>(n)->getAnimations())
            if (!anim.isNull() && anim->hasSkeletalAnimation()) return n;
        const int kids = const_cast<SceneNode *>(n)->childCount();
        for (int i = 0; i < kids; ++i)
            if (const SceneNode *hit = findHost(const_cast<SceneNode *>(n)->childAt(i)))
                return hit;
        return nullptr;
    };
    const SceneNode *host = findHost(node.data());
    if (!host) return;
    for (const auto &anim : const_cast<SceneNode *>(host)->getAnimations()) {
        if (anim.isNull() || !anim->hasSkeletalAnimation()) continue;
        // FIRST WINS on a duplicate name, matching the engine's uniquifying
        // attach: two clips with one name is a content defect, not a crash.
        if (!out.contains(anim->getName())) out.insert(anim->getName(), anim->getLength());
    }
}

// ---------------------------------------------------------------------------
// THE EVALUATOR

AvatarLocomotion::AvatarLocomotion() {}

bool AvatarLocomotion::setAsset(const LocomotionAsset &asset, QString *error)
{
    // The VERB path refuses an empty asset outright; only the degradation path
    // (nothing role-bound) is allowed to install one, through the other setter.
    if (!asset.validate(error)) return false;
    if (!setAssetPreservingDefaultFlag(asset, error)) return false;
    // AUTHORED, from here on: a user (or a script) said what this character's
    // state graph is, so no later role bind or knob change may regenerate over
    // the top of it.
    mIsDefault = false;
    return true;
}

bool AvatarLocomotion::restoreAsset(const LocomotionAsset &asset, bool isDefault, QString *error)
{
    if (!setAssetPreservingDefaultFlag(asset, error)) return false;
    mIsDefault = isDefault;
    return true;
}

bool AvatarLocomotion::setAssetPreservingDefaultFlag(const LocomotionAsset &asset, QString *error)
{
    if (asset.isEmpty()) {
        // An empty asset is a legitimate DEGRADATION (§7.4: nothing bound), not
        // a refusal — but it must not go through validate(), which correctly
        // refuses a stateless asset when a user hands one in.
        mAsset = asset;
        reset();
        return true;
    }
    if (!asset.validate(error)) return false;
    mAsset = asset;
    reset();
    return true;
}

void AvatarLocomotion::reset()
{
    mCurrent = mAsset.entry;
    mPrevious.clear();
    mPhase = 0.0f;
    mPrevPhase = 0.0f;
    mTransitioning = false;
    mBlendElapsed = 0.0f;
    mBlendDuration = 0.0f;
    mBlendU = 1.0f;
    mWeights.clear();
}

void AvatarLocomotion::setClipRole(ClipRole role, const QString &clip)
{
    mRoles.set(role, clip);
    mRoleIsManual[int(role)] = true;
    if (mIsDefault) {
        mAsset = buildDefaultLocomotionAsset(mRoles, mDefaultWalkSpeed, mDefaultRunSpeed);
        reset();
    }
}

void AvatarLocomotion::bindRolesFromClips(const QStringList &clipNames, float walkSpeed,
                                          float runSpeed)
{
    mDefaultWalkSpeed = walkSpeed;
    mDefaultRunSpeed = runSpeed;
    const ClipRoles matched = matchClipRoles(clipNames);
    for (int i = 0; i < kClipRoleCount; ++i) {
        // A role the user bound by hand (or one a scene file recorded) is NOT
        // clobbered by a later auto-match: rebinding is what happens whenever a
        // clip is loaded onto an already-spawned character, and silently
        // discarding an explicit choice there would be a papercut nobody could
        // diagnose.
        if (mRoleIsManual[i]) continue;
        mRoles.names[i] = matched.names[i];
    }
    if (mIsDefault) {
        mAsset = buildDefaultLocomotionAsset(mRoles, walkSpeed, runSpeed);
        reset();
    }
}

void AvatarLocomotion::refreshDefaultAsset(float walkSpeed, float runSpeed)
{
    mDefaultWalkSpeed = walkSpeed;
    mDefaultRunSpeed = runSpeed;
    if (!mIsDefault) return;
    const QString wasIn = mCurrent;
    mAsset = buildDefaultLocomotionAsset(mRoles, walkSpeed, runSpeed);
    // The blend-space POSITIONS moved, not the state graph, so a running
    // character must not be thrown back to the entry state by a knob edit.
    if (mAsset.findState(wasIn)) {
        mCurrent = wasIn;
        mPrevious.clear();
        mTransitioning = false;
        mBlendU = 1.0f;
    } else {
        reset();
    }
}

void AvatarLocomotion::markRolesFromFile(const ClipRoles &roles, bool isDefaultAsset)
{
    mRoles = roles;
    for (int i = 0; i < kClipRoleCount; ++i)
        mRoleIsManual[i] = !roles.names[i].isEmpty();
    mIsDefault = isDefaultAsset;
}

void AvatarLocomotion::setDefaultAsset(const ClipRoles &roles, float walkSpeed, float runSpeed)
{
    mRoles = roles;
    mDefaultWalkSpeed = walkSpeed;
    mDefaultRunSpeed = runSpeed;
    mAsset = buildDefaultLocomotionAsset(roles, walkSpeed, runSpeed);
    mIsDefault = true;
    reset();
}

void AvatarLocomotion::sampleState(const LocomotionStateDef &st, float speed,
                                   const QMap<QString, float> &clipLengths,
                                   QVector<Sample> &out) const
{
    out.clear();
    auto lengthOf = [&clipLengths](const QString &clip) {
        // A clip the document does not know is ZERO-LENGTH here. It is still
        // published (the boundary pads a zero-length clip rather than refusing
        // it, Types.h:138-141) but it is excluded from the blended cycle length
        // and from phase advance — §7.3's rule, and the reason a Mixamo
        // character's single-frame "mixamo.com" T-pose cannot divide by zero.
        const auto it = clipLengths.constFind(clip);
        return it == clipLengths.constEnd() ? 0.0f : std::max(0.0f, *it);
    };

    if (st.kind == LocomotionSourceKind::Clip) {
        Sample s;
        s.clip = st.clip;
        s.weight = 1.0f;
        s.length = lengthOf(st.clip);
        s.authoredSpeed = 0.0f;
        out.append(s);
        return;
    }

    const auto &sm = st.space.samples;
    if (sm.isEmpty()) return;
    out.reserve(sm.size());
    for (const auto &b : sm) {
        Sample s;
        s.clip = b.clip;
        s.weight = 0.0f;
        s.length = lengthOf(b.clip);
        s.authoredSpeed = b.authoredSpeed;
        out.append(s);
    }

    // BRACKET, clamp, lerp — and NO EXTRAPOLATION, ever (§7.3): below the first
    // sample or above the last, one clip carries weight 1.
    const float v = std::min(std::max(speed, sm.first().position), sm.last().position);
    if (v <= sm.first().position) { out[0].weight = 1.0f; return; }
    if (v >= sm.last().position) { out.last().weight = 1.0f; return; }
    for (int i = 0; i + 1 < sm.size(); ++i) {
        if (v >= sm[i].position && v <= sm[i + 1].position) {
            const float span = sm[i + 1].position - sm[i].position;
            const float t = span > 0.0f ? (v - sm[i].position) / span : 0.0f;
            out[i].weight = 1.0f - t;
            out[i + 1].weight = t;
            return;
        }
    }
    out.last().weight = 1.0f;   // unreachable with a validated space; never empty (R2)
}

float AvatarLocomotion::advancePhase(const LocomotionStateDef &st, const QVector<Sample> &samples,
                                     float phase, float speed, float dt)
{
    if (samples.isEmpty() || dt <= 0.0f) return phase;

    float delta = 0.0f;
    if (st.kind == LocomotionSourceKind::Clip) {
        const float len = samples.first().length;
        if (len <= 0.0f) return phase;
        delta = dt * st.speed / len;
    } else {
        // CLIP SYNC (§7.3 / §B.7 rule 2): all samples share ONE normalized
        // phase, and the blended cycle length is the weighted sum of the
        // lengths. Zero-length samples are excluded from L and from sync.
        float L = 0.0f;
        float authored = 0.0f;
        for (const auto &s : samples) {
            if (s.length > 0.0f) L += s.weight * s.length;
            authored += s.weight * s.authoredSpeed;
        }
        if (L <= 0.0f) return phase;
        // `authoredSpeed` earns its keep here and nowhere else: without it,
        // raising runSpeed from 5.5 to 8 leaves the run clip playing at its
        // authored rate while the capsule moves 45% faster, and the feet slide.
        float rate = 1.0f;
        if (authored > 1e-4f) rate = std::min(2.0f, std::max(0.5f, speed / authored));
        delta = dt * rate / L;
    }

    float next = phase + delta;
    if (st.loop) {
        if (next >= 1.0f) next = std::fmod(next, 1.0f);
        if (next < 0.0f) next = 0.0f;
    } else {
        next = std::min(1.0f, std::max(0.0f, next));
    }
    return next;
}

void AvatarLocomotion::emitWeights(const LocomotionStateDef &st, const QVector<Sample> &samples,
                                   float phase, float scale)
{
    for (const auto &s : samples) {
        const float w = s.weight * scale;
        // ABSOLUTE seconds into the clip — never a relative advance (R1). A
        // synced blend space puts every sample at the SAME normalized phase, so
        // a 1.0 s walk and a 0.6 s run at 0.5/0.5 sit at 0.5 s and 0.3 s.
        const float t = s.length > 0.0f ? phase * s.length : 0.0f;
        bool merged = false;
        for (auto &existing : mWeights) {
            if (existing.clip != s.clip) continue;
            // The SAME clip in both halves of a cross-fade (a walk that appears
            // in the source and the destination) is one engine clip, so the
            // weights add; the time comes from the heavier contribution, since
            // one clip cannot be at two times.
            if (w > existing.weight) existing.time = t;
            existing.weight += w;
            merged = true;
            break;
        }
        if (merged) continue;
        LocomotionClipWeight cw;
        cw.clip = s.clip;
        cw.weight = w;
        cw.time = t;
        cw.looping = st.loop;
        mWeights.append(cw);
    }
}

bool AvatarLocomotion::conditionHolds(const LocomotionCondition &c,
                                      const AvatarLocomotionState &params, float phase) const
{
    switch (c.kind) {
    case LocomotionConditionKind::SpeedGreater:  return params.speed > c.value;
    case LocomotionConditionKind::SpeedLess:     return params.speed < c.value;
    case LocomotionConditionKind::Grounded:      return params.grounded;
    case LocomotionConditionKind::NotGrounded:   return !params.grounded;
    case LocomotionConditionKind::ClipEnded:     return phase >= c.value;
    case LocomotionConditionKind::JumpRequested: return params.jumpRequested;
    }
    return false;
}

void AvatarLocomotion::step(const AvatarLocomotionState &params,
                            const QMap<QString, float> &clipLengths, float dt)
{
    if (mAsset.isEmpty()) {
        mWeights.clear();
        mCurrent.clear();
        mPrevious.clear();
        return;
    }
    const LocomotionStateDef *cur = mAsset.findState(mCurrent);
    if (!cur) {
        reset();
        cur = mAsset.findState(mCurrent);
        if (!cur) { mWeights.clear(); return; }
    }

    // ---- 1. the blend clock, FIRST -----------------------------------------
    //
    // The transition was TAKEN at the bottom of an earlier step and published
    // at u = 0 there, so a 0.25 s blend completes exactly 15 steps later at
    // dt = 1/60 (gate S2). The epsilon is 0.06% of one such step: it absorbs
    // the float drift of summing 1/60 fifteen times (0.25000002) without ever
    // being able to end a blend a whole frame early.
    if (mTransitioning) {
        mBlendElapsed += dt;
        if (mBlendDuration <= 0.0f || mBlendElapsed + 1e-5f >= mBlendDuration) {
            mTransitioning = false;
            mBlendU = 1.0f;
            mPrevious.clear();
            mPrevPhase = 0.0f;
        } else {
            mBlendU = mBlendElapsed / mBlendDuration;
        }
    }

    // ---- 2. phases ---------------------------------------------------------
    QVector<Sample> curSamples;
    sampleState(*cur, params.speed, clipLengths, curSamples);
    mPhase = advancePhase(*cur, curSamples, mPhase, params.speed, dt);

    const LocomotionStateDef *prev = mTransitioning ? mAsset.findState(mPrevious) : nullptr;
    QVector<Sample> prevSamples;
    if (prev) {
        // BOTH states evaluate during a transition (§7.1) — the outgoing one
        // keeps running rather than freezing on the frame it was left, which is
        // the difference between a cross-fade and a snap-with-a-ghost.
        sampleState(*prev, params.speed, clipLengths, prevSamples);
        mPrevPhase = advancePhase(*prev, prevSamples, mPrevPhase, params.speed, dt);
    }

    // ---- 3. transitions, FIRST MATCH WINS in list order --------------------
    //
    // A transition already in progress is NOT re-evaluated until it completes
    // (§7.1), so a condition that flickers cannot restart the same blend every
    // step.
    if (!mTransitioning) {
        for (const auto &t : mAsset.transitions) {
            if (t.to == mCurrent) continue;
            if (t.from != QLatin1String("*")) {
                if (t.from != mCurrent) continue;
            } else if (hasExplicitTransition(mCurrent, t.to)) {
                // THE WILDCARD MEANS "ANY OTHER STATE", and this line is what
                // makes §7.2's table work as written. Its transition 3 is
                // `* -> Fall` on `!grounded`; without this, that row would fire
                // on the very first airborne step of JumpStart — whose own
                // `clipEnded(0.9)` row is still false — and the jump-start clip
                // would be cut to one frame, silently. A state that has an
                // EXPLICIT route to the same destination has already said how
                // it gets there, so the wildcard does not speak for it. (Unreal
                // draws the same fence around its Any State node.)
                continue;
            }
            if (!conditionHolds(t.condition, params, mPhase)) continue;

            mPrevious = mCurrent;
            mPrevPhase = mPhase;
            prev = cur;
            prevSamples = curSamples;
            mCurrent = t.to;
            mPhase = 0.0f;
            cur = mAsset.findState(mCurrent);
            sampleState(*cur, params.speed, clipLengths, curSamples);
            mBlendDuration = t.blendDuration;
            mBlendElapsed = 0.0f;
            if (t.blendDuration > 0.0f) {
                mTransitioning = true;
                mBlendU = 0.0f;   // published at 0 on the step it is taken
            } else {
                mTransitioning = false;   // an instant cut
                mBlendU = 1.0f;
                mPrevious.clear();
                prev = nullptr;
                prevSamples.clear();
            }
            break;
        }
    }

    // ---- 4. the push -------------------------------------------------------
    mWeights.clear();
    const float u = mTransitioning ? mBlendU : 1.0f;
    emitWeights(*cur, curSamples, mPhase, u);
    if (mTransitioning && prev) emitWeights(*prev, prevSamples, mPrevPhase, 1.0f - u);

    // R2, the last line of defence: an all-zero set is refused by the boundary
    // and an EMPTY set freezes the pose. Neither is reachable with a validated
    // asset, so if it happens the asset is the bug — say so once and publish
    // something rather than freezing a character mid-stride.
    float total = 0.0f;
    for (const auto &w : mWeights) total += w.weight;
    if (mWeights.isEmpty() || total <= 1e-6f) {
        if (!curSamples.isEmpty()) {
            mWeights.clear();
            LocomotionClipWeight cw;
            cw.clip = curSamples.first().clip;
            cw.weight = 1.0f;
            cw.time = curSamples.first().length > 0.0f ? mPhase * curSamples.first().length : 0.0f;
            cw.looping = cur->loop;
            mWeights.append(cw);
        }
    }
}

bool AvatarLocomotion::hasExplicitTransition(const QString &from, const QString &to) const
{
    for (const auto &t : mAsset.transitions)
        if (t.from == from && t.to == to) return true;
    return false;
}

void AvatarLocomotion::stepForNode(const SceneNodePtr &node, const AvatarLocomotionState &params,
                                   float dt, float walkSpeed, float runSpeed)
{
    refreshClips(node, walkSpeed, runSpeed);
    step(params, mClipLengths, dt);
}

void AvatarLocomotion::refreshClips(const SceneNodePtr &node, float walkSpeed, float runSpeed)
{
    // A CHEAP signature, computed without allocating: the clip table is rebuilt
    // only when the document's clip set actually moved. Rebuilding it every
    // frame would be correct but would allocate per avatar per frame for a set
    // that changes when a file is loaded and at no other time.
    QMap<QString, float> fresh;
    collectAvatarClips(node, fresh);
    if (fresh == mClipLengths) return;
    mClipLengths = fresh;
    if (mIsDefault) {
        // A clip loaded onto an already-spawned character re-matches the roles
        // it did not have and regenerates the default asset. Manual bindings
        // survive (bindRolesFromClips skips them).
        //
        // NOTE that QMap::keys() is ALPHABETICAL, not the importer's order, and
        // that is a determinism choice rather than an accident: when two clips
        // both score for one role (a "Walking" and a "Walk Forward"), the
        // matcher's tie-break is list order, so the list has to be one every
        // machine and every load agrees on. The clip table's insertion order is
        // the importer's, and that is not stable across formats.
        bindRolesFromClips(mClipLengths.keys(), walkSpeed, runSpeed);
    }
}

}   // namespace iris
