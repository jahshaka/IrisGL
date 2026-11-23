/**************************************************************************
This file is part of JahshakaVR, VR Authoring Toolkit
http://www.jahshaka.com
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/input/inputmap.h"

#include "document/input/keyvaluestore.h"
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include "core/text.h"

namespace iris {

namespace {

const char *kActionNames[kInputActionCount] = { "Move", "Look", "Jump", "Sprint" };

std::string encodeKey(const InputKeyBinding &b, bool axis)
{
    if (b.key == 0) return std::string();
    if (!axis) return iris::text::cat(b.key);
    return iris::text::cat(b.key, ":", b.x, ",", b.y);   // a contribution in its shortest round-trip text
}

bool decodeKey(const std::string &text, bool axis, InputKeyBinding &out)
{
    std::string name = text;
    float x = 0.f, y = 0.f;
    if (axis) {
        const size_t colon = text.rfind(':');
        if (colon == std::string::npos) return false;
        name = text.substr(0, colon);
        const std::vector<std::string> parts = iris::text::split(std::string_view(text).substr(colon + 1), ',');
        if (parts.size() != 2) return false;
        const std::optional<double> px = iris::text::parseDouble(parts[0]), py = iris::text::parseDouble(parts[1]);
        if (!px || !py) return false;
        x = float(*px);
        y = float(*py);
    }
    const std::optional<std::int64_t> code = iris::text::parseInt(name);   // the key's number: never a name
    if (!code || *code <= 0 || *code > 0x7fffffff) return false;
    const int key = int(*code);
    out = InputKeyBinding{ key, x, y };
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// InputMap
// ---------------------------------------------------------------------------

InputMap::InputMap() { resetToDefaults(); }

InputMap InputMap::defaults() { return InputMap(); }

void InputMap::resetToDefaults()
{
    // §8.2's table, and the owner's words: "space to jump, W to walk".
    // Y is FORWARD (+1 = forward, matching the spec's "W/S = +Y/−Y"); the
    // camera-relative rotation of this vector is possession's job (Stage 3),
    // never the producer's.
    //
    // THE ARROWS ARE BOUND TOO (owner decision 2026-09-09). The EDITOR's fly
    // moved off W/A/S/D onto the arrow cluster, freeing the letters for tool
    // shortcuts; the PLAYER deliberately kept both spellings, because a player
    // is a game surface and a hand arriving from the editor's arrows must not
    // have to change grip to walk. They are ordinary bindings on the one Move
    // action, so `input.bindings()` lists them, `input.bind` replaces them and
    // `input.resetBindings` brings them back — an alias table would have been
    // a second, invisible source of truth.
    mBindings[int(InputAction::Move)] = {
        { inputkeys::W,     0.f, +1.f },
        { inputkeys::S,     0.f, -1.f },
        { inputkeys::A,    -1.f,  0.f },
        { inputkeys::D,    +1.f,  0.f },
        { inputkeys::Up,    0.f, +1.f },
        { inputkeys::Down,  0.f, -1.f },
        { inputkeys::Left, -1.f,  0.f },
        { inputkeys::Right,+1.f,  0.f },
    };
    // Look is the mouse. An empty key list here is the DEFAULT, not "unbound"
    // — a user may still bind keys to it (arrow-key look) and they will sum
    // with the mouse delta.
    mBindings[int(InputAction::Look)].clear();
    mBindings[int(InputAction::Jump)]   = { { inputkeys::Space, 0.f, 0.f } };
    mBindings[int(InputAction::Sprint)] = { { inputkeys::Shift, 0.f, 0.f } };
}

InputActionType InputMap::typeOf(InputAction a)
{
    switch (a) {
    case InputAction::Move:
    case InputAction::Look:   return InputActionType::Axis2D;
    case InputAction::Jump:
    case InputAction::Sprint: return InputActionType::Button;
    }
    return InputActionType::Button;
}

bool InputMap::isLatched(InputAction a) { return a == InputAction::Jump; }
bool InputMap::usesMouse(InputAction a) { return a == InputAction::Look; }

std::string InputMap::actionName(InputAction a)
{
    const int i = int(a);
    if (i < 0 || i >= kInputActionCount) return std::string();
    return kActionNames[i];
}

bool InputMap::actionFromName(const std::string &name, InputAction &out)
{
    for (int i = 0; i < kInputActionCount; ++i) {
        if (iris::text::iequalsAscii(name, kActionNames[i])) {
            out = InputAction(i);
            return true;
        }
    }
    return false;
}

std::vector<InputAction> InputMap::allActions()
{
    return { InputAction::Move, InputAction::Look, InputAction::Jump, InputAction::Sprint };
}

const std::vector<InputKeyBinding> &InputMap::bindings(InputAction a) const
{
    return mBindings[int(a)];
}

bool InputMap::bind(InputAction a, const std::vector<InputKeyBinding> &keys,
                    std::string *conflictAction, int *conflictKey)
{
    for (const auto &b : keys) {
        if (b.key == 0) continue;
        for (int i = 0; i < kInputActionCount; ++i) {
            if (i == int(a)) continue;
            for (const auto &other : mBindings[i]) {
                if (other.key != b.key) continue;
                if (conflictAction) *conflictAction = actionName(InputAction(i));
                if (conflictKey)    *conflictKey    = b.key;
                return false;   // nothing written — the refusal is atomic
            }
        }
    }
    std::vector<InputKeyBinding> clean;
    clean.reserve(keys.size());
    for (const auto &b : keys)
        if (b.key != 0) clean.push_back(b);
    mBindings[int(a)] = clean;
    return true;
}

void InputMap::unbind(InputAction a) { mBindings[int(a)].clear(); }

bool InputMap::isBound(int key) const
{
    if (key == 0) return false;
    for (int i = 0; i < kInputActionCount; ++i)
        for (const auto &b : mBindings[i])
            if (b.key == key) return true;
    return false;
}

InputAction InputMap::actionForKey(int key, bool *found) const
{
    for (int i = 0; i < kInputActionCount; ++i)
        for (const auto &b : mBindings[i])
            if (b.key == key) {
                if (found) *found = true;
                return InputAction(i);
            }
    if (found) *found = false;
    return InputAction::Move;
}

Value InputMap::toValue() const
{
    Value::Object m;
    for (int i = 0; i < kInputActionCount; ++i) {
        const InputAction a = InputAction(i);
        const bool axis = typeOf(a) == InputActionType::Axis2D;
        Value::Array encoded;
        for (const auto &b : mBindings[i]) {
            const std::string s = encodeKey(b, axis);
            if (!s.empty()) encoded.push_back(Value(s));
        }
        m.emplace_back(actionName(a), Value(std::move(encoded)));
    }
    return Value(std::move(m));
}

bool InputMap::fromValue(const Value &m)
{
    bool understoodAny = false;
    if (m.kind() != Kind::Object) return false;
    for (const Value::Member &member : m.asObject()) {
        InputAction a;
        if (!actionFromName(member.first, a)) continue;   // tolerant: a future action
        const bool axis = typeOf(a) == InputActionType::Axis2D;
        std::vector<InputKeyBinding> keys;
        // The rows: an Array of Strings; ONE String is one row (a one-entry list the settings file hands back as a
        // single string, which QVariant::toStringList read the same way). An element that is NOT a String is skipped
        // where toStringList converted it (5 -> "5"): only a hand-made value can carry one (the lead's F15).
        std::vector<std::string> rows;
        if (member.second.kind() == Kind::String) rows.push_back(member.second.asString());
        if (member.second.kind() == Kind::Array)
            for (const Value &row : member.second.asArray())
                if (row.kind() == Kind::String) rows.push_back(row.asString());
        for (const std::string &row : rows) {
            InputKeyBinding b;
            if (decodeKey(row, axis, b)) keys.push_back(b);
        }
        if (!rows.empty() && keys.empty()) continue;   // every row undecodable: the action keeps what it has
        mBindings[int(a)] = keys;
        understoodAny = true;
    }
    return understoodAny;
}

void InputMap::load(const KeyValueStore &settings)
{
    Value::Object m;
    for (int i = 0; i < kInputActionCount; ++i) {
        const std::string name = actionName(InputAction(i));
        const std::optional<Value> row = settings.value("input/" + name);
        if (!row) continue;                       // no row = keep the default
        m.emplace_back(name, *row);
    }
    if (!m.empty()) fromValue(Value(std::move(m)));
}

void InputMap::save(KeyValueStore &settings) const
{
    const Value m = toValue();
    for (const Value::Member &member : m.asObject())
        settings.setValue("input/" + member.first, member.second);
}

void InputMap::clearPersisted(KeyValueStore &settings)
{
    for (int i = 0; i < kInputActionCount; ++i)
        settings.remove("input/" + actionName(InputAction(i)));
}

// ---------------------------------------------------------------------------
// InputSystem
// ---------------------------------------------------------------------------

InputSystem &InputSystem::instance()
{
    static InputSystem sInstance;
    return sInstance;
}

void InputSystem::recomputeHeld()
{
    InputAxis2D move;
    bool sprint = false;
    for (int held : mHeld) {
        for (const auto &b : mMap.bindings(InputAction::Move)) {
            if (b.key == held) { move.x += b.x; move.y += b.y; }
        }
        for (const auto &b : mMap.bindings(InputAction::Sprint)) {
            if (b.key == held) sprint = true;
        }
    }
    // §5: clamped to the unit disc, so a diagonal is not 1.41x faster than a
    // straight line. Opposite keys cancel to zero first, which is what makes
    // "A and D together" a stop rather than a drift.
    const float len = std::sqrt(move.x * move.x + move.y * move.y);
    if (len > 1.f) { move.x /= len; move.y /= len; }
    mState.move = move;
    mState.sprint = sprint;
}

bool InputSystem::keyPressed(int key)
{
    if (!mMap.isBound(key)) return false;
    const bool wasHeld = (mHeld.count(key) != 0);
    mHeld.insert(key);
    if (!wasHeld) {
        // The LATCH edge. A second press with no release in between (an
        // auto-repeat the caller failed to filter) cannot reach here.
        for (const auto &b : mMap.bindings(InputAction::Jump))
            if (b.key == key) { mState.jump = true; break; }
    }
    recomputeHeld();
    return true;
}

bool InputSystem::keyReleased(int key)
{
    if (!mMap.isBound(key)) return false;
    mHeld.erase(key);
    recomputeHeld();
    return true;
}

void InputSystem::mouseMoved(float dx, float dy)
{
    mState.look.x += dx;
    mState.look.y += dy;
}

void InputSystem::clearKeys()
{
    mHeld.clear();
    mState.clear();
}

bool InputSystem::consumeJump()
{
    const bool j = mState.jump;
    mState.jump = false;
    return j;
}

InputAxis2D InputSystem::consumeLook()
{
    const InputAxis2D l = mState.look;
    mState.look = InputAxis2D();
    return l;
}

void InputSystem::setMove(float x, float y)
{
    const float len = std::sqrt(x * x + y * y);
    if (len > 1.f) { x /= len; y /= len; }
    mState.move.x = x;
    mState.move.y = y;
}

void InputSystem::setLook(float x, float y) { mState.look.x = x; mState.look.y = y; }
void InputSystem::setSprint(bool on)        { mState.sprint = on; }
void InputSystem::requestJump()             { mState.jump = true; }

void InputSystem::setSettings(KeyValueStore *settings)
{
    mSettings = settings;
    if (mSettings) mMap.load(*mSettings);
}

bool InputSystem::bind(InputAction a, const std::vector<InputKeyBinding> &keys,
                       std::string *conflictAction, int *conflictKey)
{
    if (!mMap.bind(a, keys, conflictAction, conflictKey)) return false;
    // A rebind can orphan a held key (the user rebound the key they are holding
    // — or, far more likely, a test did). Recompute rather than leave a phantom.
    recomputeHeld();
    if (mSettings) mMap.save(*mSettings);
    return true;
}

void InputSystem::resetBindings()
{
    mMap.resetToDefaults();
    recomputeHeld();
    // REMOVE rather than write the defaults: a settings file that carries no
    // input rows is the honest record of "this user never rebound anything",
    // and it is what lets a suite restore a SHARED jahsettings.ini exactly.
    if (mSettings) InputMap::clearPersisted(*mSettings);
}

bool gameplayClaimsKey(bool playing, int key)
{
    return playing && InputSystem::instance().map().isBound(key);
}

void InputSystem::resetForTest()
{
    mSettings = nullptr;
    mMap.resetToDefaults();
    mHeld.clear();
    mState.clear();
}

} // namespace iris
