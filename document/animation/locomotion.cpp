/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/animation/locomotion.h"

#include "document/properties/valueshapes.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

#include "document/animation/animation.h"
#include "document/physics/avatarmovement.h"
#include "document/scenegraph/scenenode.h"
#include "core/text.h"

namespace iris
{

namespace
{
/// The float spelling used in every canonical condition string. 'g' with 6
/// significant digits round-trips a float exactly for every value a human
/// types and never grows an exponent for the range conditions live in.
std::string num(float v)
{
    // (the ruling's call 3: a local std::to_chars) - QString::number(v, 'g', 6)'s text: printf's %g at 6 digits
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, double(v), std::chars_format::general, 6);
    return std::string(buf, r.ptr);
}

/// QString::toFloat's answer for the grammar's numbers: the whole text, a leading '+' allowed (Qt took one).
std::optional<float> parseNumber(std::string_view s)
{
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    const std::optional<double> d = iris::text::parseDouble(s);
    if (!d) return std::nullopt;
    return float(*d);
}

/// The whole vocabulary, in the refusal message. A closed vocabulary is only
/// usable if being refused tells you what the alternatives are (gate S5).
const char *kVocabulary =
    "speed > x, speed < x, grounded, !grounded, clipEnded(fraction), jumpRequested";
}   // namespace

// ---------------------------------------------------------------------------
// CONDITIONS

std::string LocomotionCondition::text() const
{
    switch (kind) {
    case LocomotionConditionKind::SpeedGreater: return "speed > " + num(value);
    case LocomotionConditionKind::SpeedLess:    return "speed < " + num(value);
    case LocomotionConditionKind::Grounded:     return "grounded";
    case LocomotionConditionKind::NotGrounded:  return "!grounded";
    case LocomotionConditionKind::ClipEnded:
        return "clipEnded(" + num(value) + ')';
    case LocomotionConditionKind::JumpRequested: return "jumpRequested";
    }
    return std::string();
}

bool LocomotionCondition::parse(const std::string &text, LocomotionCondition &out, std::string *error)
{
    auto refuse = [&](const std::string &why) {
        if (error)
            *error = iris::text::cat("unrecognised condition \"", std::string(iris::text::trim(text)), "\": ", why, " — the condition vocabulary is CLOSED, and the six forms are: ", kVocabulary);
        return false;
    };

    // Whitespace is not significant anywhere in the grammar, so it is removed
    // once rather than handled per form. Case is not significant either: a user
    // who typed `ClipEnded` meant clipEnded.
    std::string c = text;
    c.erase(std::remove_if(c.begin(), c.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }), c.end());
    if (c.empty()) return refuse("it is empty");
    const std::string lower = iris::text::asciiLower(c);

    if (lower == "grounded") {
        out.kind = LocomotionConditionKind::Grounded;
        out.value = 0.0f;
        return true;
    }
    if (lower == "!grounded") {
        out.kind = LocomotionConditionKind::NotGrounded;
        out.value = 0.0f;
        return true;
    }
    if (lower == "jumprequested") {
        out.kind = LocomotionConditionKind::JumpRequested;
        out.value = 0.0f;
        return true;
    }
    if (iris::text::startsWith(lower, "speed>") || iris::text::startsWith(lower, "speed<")) {
        const bool greater = lower.at(5) == '>';
        const std::optional<float> parsed = parseNumber(std::string_view(lower).substr(6));
        const float v = parsed ? *parsed : 0.0f;
        if (!parsed || !std::isfinite(v))
            return refuse(iris::text::cat("'", c.substr(6), "' is not a number"));
        out.kind = greater ? LocomotionConditionKind::SpeedGreater
                           : LocomotionConditionKind::SpeedLess;
        out.value = v;
        return true;
    }
    if (iris::text::startsWith(lower, "clipended(")) {
        if (!iris::text::endsWith(lower, ")"))
            return refuse("the closing parenthesis is missing");
        const std::string inner = lower.substr(10, lower.size() - 11);
        const std::optional<float> parsed = parseNumber(inner);
        const float v = parsed ? *parsed : 0.0f;
        if (!parsed || !std::isfinite(v))
            return refuse(iris::text::cat("'", inner, "' is not a number"));
        // A fraction OF THE CLIP. Outside [0,1] it either fires on the first
        // step or never fires at all, and both read as a broken state machine.
        if (v < 0.0f || v > 1.0f)
            return refuse(iris::text::cat("the fraction ", num(v), " is outside [0, 1]"));
        out.kind = LocomotionConditionKind::ClipEnded;
        out.value = v;
        return true;
    }
    // `speed >= x`, `speed == x`, `!jumpRequested`, `speed > x && grounded` and
    // every other near-miss lands here, by design: the vocabulary is the fence.
    return refuse("no form of the vocabulary matches it");
}

// ---------------------------------------------------------------------------
// THE ASSET

const LocomotionStateDef *LocomotionAsset::findState(const std::string &n) const
{
    for (const auto &s : states)
        if (s.name == n) return &s;
    return nullptr;
}

bool LocomotionAsset::validate(std::string *error) const
{
    auto fail = [&](const std::string &msg) {
        if (error) *error = msg;
        return false;
    };

    if (states.empty()) return fail("the asset has no states");

    for (int i = 0; i < states.size(); ++i) {
        const LocomotionStateDef &s = states[i];
        if (s.name.empty())
            return fail(iris::text::cat("state ", i, " has no name"));
        if (s.name == "*")
            return fail(iris::text::cat("state ", i, " is named '*', which is the wildcard a transition's `from` uses"));
        for (int j = 0; j < i; ++j)
            if (states[j].name == s.name)
                return fail(iris::text::cat("two states are named '", s.name, "'"));

        // R2: "no clip enabled" is a FROZEN pose, not a bind pose. A state that
        // cannot resolve to at least one clip produces a character stuck in a
        // half-pose with no error anywhere, so it is refused here rather than
        // discovered in a demo.
        if (s.kind == LocomotionSourceKind::Clip) {
            if (s.clip.empty())
                return fail(iris::text::cat("state '", s.name, "' names no clip"));
            if (!(s.speed > 0.0f))
                return fail(iris::text::cat("state '", s.name, "' has a play speed of ", num(s.speed), "; it must be positive"));
        } else {
            const auto &sm = s.space.samples;
            if (sm.size() < 2)
                return fail(iris::text::cat("state '", s.name, "' is a blend space with ", sm.size(), " sample(s); a blend space needs at least 2"));
            for (int k = 0; k < sm.size(); ++k) {
                if (sm[k].clip.empty())
                    return fail(iris::text::cat("state '", s.name, "' blend sample ", k, " names no clip"));
                if (k > 0 && !(sm[k].position > sm[k - 1].position))
                    return fail(iris::text::cat("state '", s.name, "' blend sample positions must be STRICTLY increasing: sample ", k - 1, " sits at ", num(sm[k - 1].position), ", sample ", k, " at ", num(sm[k].position)));
                if (sm[k].authoredSpeed < 0.0f)
                    return fail(iris::text::cat("state '", s.name, "' blend sample ", k, " has a negative authoredSpeed"));
            }
        }
    }

    if (entry.empty()) return fail("the asset names no entry state");
    if (!findState(entry))
        return fail(iris::text::cat("the entry state '", entry, "' is not one of the asset's states"));

    for (int i = 0; i < transitions.size(); ++i) {
        const LocomotionTransitionDef &t = transitions[i];
        if (t.from.empty())
            return fail(iris::text::cat("transition ", i, " has no 'from'"));
        if (t.from != "*" && !findState(t.from))
            return fail(iris::text::cat("transition ", i, ": 'from' names '", t.from, "', which is not a state (use '*' for any state)"));
        if (t.to.empty())
            return fail(iris::text::cat("transition ", i, " has no 'to'"));
        if (!findState(t.to))
            return fail(iris::text::cat("transition ", i, ": 'to' names '", t.to, "', which is not a state"));
        if (t.from == t.to)
            return fail(iris::text::cat("transition ", i, " goes from '", t.from, "' to itself"));
        if (t.blendDuration < 0.0f || !std::isfinite(t.blendDuration))
            return fail(iris::text::cat("transition ", i, " (", t.from, " -> ", t.to, ") has a blend duration of ", num(t.blendDuration)));
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

bool clipRoleFromName(const std::string &name, ClipRole &out)
{
    const std::string n = iris::text::asciiLower(std::string(iris::text::trim(name)));
    if (n == "idle") { out = ClipRole::Idle; return true; }
    if (n == "walk") { out = ClipRole::Walk; return true; }
    if (n == "run")  { out = ClipRole::Run; return true; }
    if (n == "jump-start" || n == "jumpstart") {
        out = ClipRole::JumpStart; return true;
    }
    if (n == "fall-loop" || n == "fallloop") {
        out = ClipRole::FallLoop; return true;
    }
    if (n == "land") { out = ClipRole::Land; return true; }
    return false;
}

int ClipRoles::boundCount() const
{
    int n = 0;
    for (int i = 0; i < kClipRoleCount; ++i)
        if (!names[i].empty()) ++n;
    return n;
}

namespace
{
struct RolePatterns
{
    ClipRole role = ClipRole::Idle;
    const char *words[8] = {};
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
std::string normalizeClipName(const std::string &name)
{
    std::string out;
    out.reserve(name.size());
    // UTF-8 bytes: an ASCII letter or digit lowercased, other ASCII a space; a byte of a non-ASCII character is kept
    // (the keywords are ASCII; a non-ASCII letter stays part of its word). Then QString::simplified: runs of spaces
    // one, none at either end.
    for (const unsigned char ch : name) {
        if (ch >= 0x80) out.push_back(char(ch));
        else if (std::isalnum(ch)) out.push_back(char(std::tolower(ch)));
        else if (!out.empty() && out.back() != ' ') out.push_back(' ');
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

/// 3 = the whole name IS the keyword, 2 = a word of it is, 1 = it contains it,
/// 0 = no match. Higher is a better binding; ties break on list order, so the
/// matcher is deterministic for a given clip list (gate S7's sibling property).
int scoreAgainst(const std::string &normalized, const std::vector<std::string> &words, const RolePatterns &p)
{
    int best = 0;
    for (int i = 0; p.words[i]; ++i) {
        const std::string_view key(p.words[i]);
        if (normalized == key) return 3;
        if (std::find(words.begin(), words.end(), key) != words.end()) best = std::max(best, 2);
        else if (normalized.find(key) != std::string::npos) best = std::max(best, 1);
    }
    return best;
}
}   // namespace

ClipRoles matchClipRoles(const std::vector<std::string> &clipNames)
{
    ClipRoles roles;
    const int n = clipNames.size();
    std::vector<std::string> normalized(n);
    std::vector<std::vector<std::string>> words(n);
    std::vector<bool> used(n, false);
    for (int i = 0; i < n; ++i) {
        normalized[i] = normalizeClipName(clipNames[i]);
        if (!normalized[i].empty()) words[i] = iris::text::split(normalized[i], ' ');   // simplified: no empty word
        if (normalized[i].empty()) used[i] = true;   // an unnamed clip binds to nothing
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
    a.name = defaultLocomotionAssetName();

    // ---- the Grounded blend space (§7.2 row 1) ----------------------------
    std::vector<LocomotionBlendSample> samples;
    auto addSample = [&](ClipRole r, float position, float authored) {
        if (!roles.has(r)) return;
        LocomotionBlendSample s;
        s.clip = roles.get(r);
        // STRICTLY increasing, enforced here rather than hoped for: a user who
        // sets walkSpeed to 0 (or runSpeed below walkSpeed) would otherwise
        // build an asset its own validator refuses.
        s.position = samples.empty() ? std::max(0.0f, position)
                                       : std::max(position, samples.back().position + 0.01f);
        s.authoredSpeed = authored;
        samples.push_back(s);
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

    const bool hasGrounded = !samples.empty();
    if (hasGrounded) {
        LocomotionStateDef grounded;
        grounded.name = "Grounded";
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
            grounded.clip = samples.front().clip;
        }
        a.states.push_back(grounded);
    }

    auto addClipState = [&](const char *name, ClipRole role, bool loop) {
        if (!roles.has(role)) return;
        LocomotionStateDef s;
        s.name = name;
        s.kind = LocomotionSourceKind::Clip;
        s.clip = roles.get(role);
        s.loop = loop;
        a.states.push_back(s);
    };
    addClipState("JumpStart", ClipRole::JumpStart, false);
    addClipState("Fall", ClipRole::FallLoop, true);
    addClipState("Land", ClipRole::Land, false);

    if (a.states.empty()) return a;   // nothing bound: an EMPTY asset, not a broken one
    a.entry = a.states.front().name;

    const bool hasJump = a.findState("JumpStart") != nullptr;
    const bool hasFall = a.findState("Fall") != nullptr;
    const bool hasLand = a.findState("Land") != nullptr;

    auto cond = [](const char *text) {
        LocomotionCondition c;
        std::string err;
        LocomotionCondition::parse(text, c, &err);   // built-in: always parses
        return c;
    };
    auto addTransition = [&](const std::string &from, const std::string &to, const char *condition,
                             float blend) {
        if (from != "*" && !a.findState(from)) return;
        if (!a.findState(to)) return;
        LocomotionTransitionDef t;
        t.from = from;
        t.to = to;
        t.condition = cond(condition);
        t.blendDuration = blend;
        a.transitions.push_back(t);
    };

    // §7.2's five transitions, IN ORDER — order is the priority, and the first
    // row is why a jump plays JumpStart rather than falling straight to Fall:
    // the movement component clears `grounded` in the same step it applies the
    // impulse, so both row 1 and row 3 hold on that step and row 1 wins.
    addTransition("Grounded", "JumpStart", "jumpRequested", 0.05f);
    if (hasFall) {
        addTransition("JumpStart", "Fall", "clipEnded(0.9)", 0.10f);
        addTransition("*", "Fall", "!grounded", 0.15f);
        if (hasLand) {
            addTransition("Fall", "Land", "grounded", 0.10f);
        } else {
            // §7.4: "No `land` -> transition 4 targets Grounded directly."
            addTransition("Fall", "Grounded", "grounded", 0.10f);
        }
    } else if (hasJump) {
        // No fall loop: the jump chain has to come back on its own, and
        // clipEnded is exactly the tool that means no user authors a timer.
        addTransition("JumpStart",
                      hasLand ? "Land" : "Grounded",
                      "clipEnded(0.9)", 0.10f);
    }
    addTransition("Land", "Grounded", "clipEnded(0.8)", 0.15f);
    return a;
}

// ---------------------------------------------------------------------------
// SERIALIZATION — ONE grammar, used by src/io AND by the verbs, so a scene file
// and `avatar.setLocomotionAsset` can never drift apart (gate S6 covers both
// paths because they are the same code).

Value locomotionAssetToJson(const LocomotionAsset &asset)
{
    const auto S = [](const std::string &q) { return Value(q); };
    Value::Array states;
    for (const auto &s : asset.states) {
        Value::Object src;
        if (s.kind == LocomotionSourceKind::Clip) {
            src.emplace_back("kind", Value("clip"));
            src.emplace_back("clip", S(s.clip));
            src.emplace_back("speed", Value(double(s.speed)));
        } else {
            src.emplace_back("kind", Value("blendspace"));
            src.emplace_back("syncClips", Value(s.space.syncClips));
            Value::Array samples;
            for (const auto &sm : s.space.samples)
                samples.push_back(Value(Value::Object{{"clip", S(sm.clip)},
                                                      {"position", Value(double(sm.position))},
                                                      {"authoredSpeed", Value(double(sm.authoredSpeed))}}));
            src.emplace_back("samples", Value(std::move(samples)));
        }
        states.push_back(Value(Value::Object{{"name", S(s.name)}, {"loop", Value(s.loop)},
                                             {"source", Value(std::move(src))}}));
    }

    Value::Array transitions;
    for (const auto &t : asset.transitions) {
        // The CANONICAL TEXT, not the enum: a file that stored an int would
        // freeze today's enum order, and a human reading the scene JSON would
        // learn nothing. `text()` round-trips through `parse` byte for byte.
        transitions.push_back(Value(Value::Object{{"from", S(t.from)}, {"to", S(t.to)},
                                                  {"condition", S(t.condition.text())},
                                                  {"blendDuration", Value(double(t.blendDuration))}}));
    }
    return Value(Value::Object{{"name", S(asset.name)}, {"entry", S(asset.entry)},
                               {"states", Value(std::move(states))}, {"transitions", Value(std::move(transitions))}});
}

bool locomotionAssetFromJson(const Value &obj, LocomotionAsset &out, std::string *error)
{
    auto fail = [&](const std::string &msg) {
        if (error) *error = msg;
        return false;
    };
    // A member that is absent or of another kind reads as QJsonValue's default did (valueshapes.h).
    const auto Q = [](const Value &o, const char *key, const std::string &d = std::string()) {
        return stringOr(o, key, d);
    };

    LocomotionAsset a;
    a.name = Q(obj, "name", defaultLocomotionAssetName());
    a.entry = Q(obj, "entry");

    const Value statesIn = arrayOr(obj, "states");   // bound: a temporary's asArray() would not outlive the loop

    for (const Value &o : statesIn.asArray()) {
        LocomotionStateDef s;
        s.name = Q(o, "name");
        s.loop = boolOr(o, "loop", true);
        const Value &src = o["source"];
        const std::string kind = iris::text::asciiLower(Q(src, "kind"));
        if (kind == "blendspace") {
            s.kind = LocomotionSourceKind::BlendSpace;
            s.space.syncClips = boolOr(src, "syncClips", true);
            const Value samplesIn = arrayOr(src, "samples");   // bound: a temporary's asArray() would not outlive the loop
            for (const Value &so : samplesIn.asArray()) {
                LocomotionBlendSample sm;
                sm.clip = Q(so, "clip");
                sm.position = float(numberOr(so, "position", 0.0));
                sm.authoredSpeed = float(numberOr(so, "authoredSpeed", 0.0));
                s.space.samples.push_back(sm);
            }
        } else if (kind == "clip" || kind.empty()) {
            s.kind = LocomotionSourceKind::Clip;
            s.clip = Q(src, "clip");
            s.speed = float(numberOr(src, "speed", 1.0));
        } else {
            return fail(iris::text::cat("state '", s.name, "': unknown source kind '", kind, "' (known: clip, blendspace)"));
        }
        a.states.push_back(s);
    }

    const Value transitions = arrayOr(obj, "transitions");
    for (int i = 0; i < int(transitions.asArray().size()); ++i) {
        const Value &o = transitions.asArray()[size_t(i)];
        LocomotionTransitionDef t;
        t.from = Q(o, "from");
        t.to = Q(o, "to");
        t.blendDuration = float(numberOr(o, "blendDuration", 0.15));
        std::string err;
        if (!LocomotionCondition::parse(Q(o, "condition"), t.condition, &err))
            return fail(iris::text::cat("transition ", i, " (", t.from, " -> ", t.to, "): ", err));
        a.transitions.push_back(t);
    }

    if (!a.validate(error)) return false;
    out = a;
    return true;
}

// ---------------------------------------------------------------------------
// CLIP TABLE

void collectAvatarClips(const SceneNodePtr &node, std::map<std::string, float> &out)
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
            if (!!anim && anim->hasSkeletalAnimation()) return n;
        const int kids = const_cast<SceneNode *>(n)->childCount();
        for (int i = 0; i < kids; ++i)
            if (const SceneNode *hit = findHost(const_cast<SceneNode *>(n)->childAt(i)))
                return hit;
        return nullptr;
    };
    const SceneNode *host = findHost(node.get());
    if (!host) return;
    for (const auto &anim : const_cast<SceneNode *>(host)->getAnimations()) {
        if (!anim || !anim->hasSkeletalAnimation()) continue;
        // FIRST WINS on a duplicate name, matching the engine's uniquifying
        // attach: two clips with one name is a content defect, not a crash.
        if (!(out.count(anim->getName()) != 0)) out.insert_or_assign(anim->getName(), anim->getLength());
    }
}

// ---------------------------------------------------------------------------
// THE EVALUATOR

AvatarLocomotion::AvatarLocomotion() {}

bool AvatarLocomotion::setAsset(const LocomotionAsset &asset, std::string *error)
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

bool AvatarLocomotion::restoreAsset(const LocomotionAsset &asset, bool isDefault, std::string *error)
{
    if (!setAssetPreservingDefaultFlag(asset, error)) return false;
    mIsDefault = isDefault;
    return true;
}

bool AvatarLocomotion::setAssetPreservingDefaultFlag(const LocomotionAsset &asset, std::string *error)
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

void AvatarLocomotion::setClipRole(ClipRole role, const std::string &clip)
{
    mRoles.set(role, clip);
    mRoleIsManual[int(role)] = true;
    if (mIsDefault) {
        mAsset = buildDefaultLocomotionAsset(mRoles, mDefaultWalkSpeed, mDefaultRunSpeed);
        reset();
    }
}

void AvatarLocomotion::bindRolesFromClips(const std::vector<std::string> &clipNames, float walkSpeed,
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
    const std::string wasIn = mCurrent;
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
        mRoleIsManual[i] = !roles.names[i].empty();
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
                                   const std::map<std::string, float> &clipLengths,
                                   std::vector<Sample> &out) const
{
    out.clear();
    auto lengthOf = [&clipLengths](const std::string &clip) {
        // A clip the document does not know is ZERO-LENGTH here. It is still
        // published (the boundary pads a zero-length clip rather than refusing
        // it, Types.h:138-141) but it is excluded from the blended cycle length
        // and from phase advance — §7.3's rule, and the reason a Mixamo
        // character's single-frame "mixamo.com" T-pose cannot divide by zero.
        const auto it = clipLengths.find(clip);
        return it == clipLengths.cend() ? 0.0f : std::max(0.0f, it->second);
    };

    if (st.kind == LocomotionSourceKind::Clip) {
        Sample s;
        s.clip = st.clip;
        s.weight = 1.0f;
        s.length = lengthOf(st.clip);
        s.authoredSpeed = 0.0f;
        out.push_back(s);
        return;
    }

    const auto &sm = st.space.samples;
    if (sm.empty()) return;
    out.reserve(sm.size());
    for (const auto &b : sm) {
        Sample s;
        s.clip = b.clip;
        s.weight = 0.0f;
        s.length = lengthOf(b.clip);
        s.authoredSpeed = b.authoredSpeed;
        out.push_back(s);
    }

    // BRACKET, clamp, lerp — and NO EXTRAPOLATION, ever (§7.3): below the first
    // sample or above the last, one clip carries weight 1.
    const float v = std::min(std::max(speed, sm.front().position), sm.back().position);
    if (v <= sm.front().position) { out[0].weight = 1.0f; return; }
    if (v >= sm.back().position) { out.back().weight = 1.0f; return; }
    for (int i = 0; i + 1 < sm.size(); ++i) {
        if (v >= sm[i].position && v <= sm[i + 1].position) {
            const float span = sm[i + 1].position - sm[i].position;
            const float t = span > 0.0f ? (v - sm[i].position) / span : 0.0f;
            out[i].weight = 1.0f - t;
            out[i + 1].weight = t;
            return;
        }
    }
    out.back().weight = 1.0f;   // unreachable with a validated space; never empty (R2)
}

float AvatarLocomotion::advancePhase(const LocomotionStateDef &st, const std::vector<Sample> &samples,
                                     float phase, float speed, float dt)
{
    if (samples.empty() || dt <= 0.0f) return phase;

    float delta = 0.0f;
    if (st.kind == LocomotionSourceKind::Clip) {
        const float len = samples.front().length;
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

void AvatarLocomotion::emitWeights(const LocomotionStateDef &st, const std::vector<Sample> &samples,
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
        mWeights.push_back(cw);
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
                            const std::map<std::string, float> &clipLengths, float dt)
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
    std::vector<Sample> curSamples;
    sampleState(*cur, params.speed, clipLengths, curSamples);
    mPhase = advancePhase(*cur, curSamples, mPhase, params.speed, dt);

    const LocomotionStateDef *prev = mTransitioning ? mAsset.findState(mPrevious) : nullptr;
    std::vector<Sample> prevSamples;
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
            if (t.from != "*") {
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
    if (mWeights.empty() || total <= 1e-6f) {
        if (!curSamples.empty()) {
            mWeights.clear();
            LocomotionClipWeight cw;
            cw.clip = curSamples.front().clip;
            cw.weight = 1.0f;
            cw.time = curSamples.front().length > 0.0f ? mPhase * curSamples.front().length : 0.0f;
            cw.looping = cur->loop;
            mWeights.push_back(cw);
        }
    }
}

bool AvatarLocomotion::hasExplicitTransition(const std::string &from, const std::string &to) const
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
    std::map<std::string, float> fresh;
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
        std::vector<std::string> names;   // the map's keys, in its (byte) order
        names.reserve(mClipLengths.size());
        for (const auto &[clipName, length] : mClipLengths) names.push_back(clipName);
        bindRolesFromClips(names, walkSpeed, runSpeed);
    }
}

}   // namespace iris
