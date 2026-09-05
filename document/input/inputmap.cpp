/**************************************************************************
This file is part of JahshakaVR, VR Authoring Toolkit
http://www.jahshaka.com
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/input/inputmap.h"

#include <QKeySequence>
#include <QSettings>
#include <cmath>

namespace iris {

namespace {

/// QKeySequence spells a BARE modifier as "Shift+" / "Ctrl+" (it treats the
/// value as a modifier with no key), which does not round-trip. Sprint's
/// default is a bare modifier, so the table is load-bearing, not cosmetic.
struct NamedKey { int key; const char *name; };
const NamedKey kNamedKeys[] = {
    { Qt::Key_Shift,   "Shift"   },
    { Qt::Key_Control, "Ctrl"    },
    { Qt::Key_Alt,     "Alt"     },
    { Qt::Key_Meta,    "Meta"    },
    { Qt::Key_Space,   "Space"   },
};

const char *kActionNames[kInputActionCount] = { "Move", "Look", "Jump", "Sprint" };

QString encodeKey(const InputKeyBinding &b, bool axis)
{
    const QString name = InputMap::keyName(b.key);
    if (name.isEmpty()) return QString();
    if (!axis) return name;
    return QStringLiteral("%1:%2,%3").arg(name).arg(b.x).arg(b.y);
}

bool decodeKey(const QString &text, bool axis, InputKeyBinding &out)
{
    QString name = text;
    float x = 0.f, y = 0.f;
    if (axis) {
        const int colon = text.lastIndexOf(':');
        if (colon < 0) return false;
        name = text.left(colon);
        const QStringList parts = text.mid(colon + 1).split(',');
        if (parts.size() != 2) return false;
        bool okX = false, okY = false;
        x = parts[0].toFloat(&okX);
        y = parts[1].toFloat(&okY);
        if (!okX || !okY) return false;
    }
    const int key = InputMap::keyFromName(name);
    if (key == 0) return false;
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
    mBindings[int(InputAction::Move)] = {
        { Qt::Key_W,  0.f, +1.f },
        { Qt::Key_S,  0.f, -1.f },
        { Qt::Key_A, -1.f,  0.f },
        { Qt::Key_D, +1.f,  0.f },
    };
    // Look is the mouse. An empty key list here is the DEFAULT, not "unbound"
    // — a user may still bind keys to it (arrow-key look) and they will sum
    // with the mouse delta.
    mBindings[int(InputAction::Look)].clear();
    mBindings[int(InputAction::Jump)]   = { { Qt::Key_Space, 0.f, 0.f } };
    mBindings[int(InputAction::Sprint)] = { { Qt::Key_Shift, 0.f, 0.f } };
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

QString InputMap::actionName(InputAction a)
{
    const int i = int(a);
    if (i < 0 || i >= kInputActionCount) return QString();
    return QString::fromLatin1(kActionNames[i]);
}

bool InputMap::actionFromName(const QString &name, InputAction &out)
{
    for (int i = 0; i < kInputActionCount; ++i) {
        if (name.compare(QString::fromLatin1(kActionNames[i]), Qt::CaseInsensitive) == 0) {
            out = InputAction(i);
            return true;
        }
    }
    return false;
}

QVector<InputAction> InputMap::allActions()
{
    return { InputAction::Move, InputAction::Look, InputAction::Jump, InputAction::Sprint };
}

QString InputMap::keyName(int key)
{
    if (key == 0) return QString();
    for (const auto &nk : kNamedKeys)
        if (nk.key == key) return QString::fromLatin1(nk.name);
    const QString s = QKeySequence(key).toString(QKeySequence::PortableText);
    return s;
}

int InputMap::keyFromName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return 0;
    for (const auto &nk : kNamedKeys)
        if (trimmed.compare(QString::fromLatin1(nk.name), Qt::CaseInsensitive) == 0)
            return nk.key;
    const QKeySequence seq = QKeySequence::fromString(trimmed, QKeySequence::PortableText);
    if (seq.count() != 1) return 0;
    // Strip the modifier bits: a gameplay binding is one physical key.
    const int key = seq[0].toCombined() & ~Qt::KeyboardModifierMask;
    // fromString does NOT fail on garbage — "Nonsense" comes back as a
    // one-element sequence holding Qt::Key_unknown (0x01ffffff). Without this
    // line every unparseable name would bind a real, unreachable key code, and
    // a corrupt settings file would silently steal a binding.
    if (key == Qt::Key_unknown) return 0;
    return key;
}

const QVector<InputKeyBinding> &InputMap::bindings(InputAction a) const
{
    return mBindings[int(a)];
}

bool InputMap::bind(InputAction a, const QVector<InputKeyBinding> &keys,
                    QString *conflictAction, QString *conflictKey)
{
    for (const auto &b : keys) {
        if (b.key == 0) continue;
        for (int i = 0; i < kInputActionCount; ++i) {
            if (i == int(a)) continue;
            for (const auto &other : mBindings[i]) {
                if (other.key != b.key) continue;
                if (conflictAction) *conflictAction = actionName(InputAction(i));
                if (conflictKey)    *conflictKey    = keyName(b.key);
                return false;   // nothing written — the refusal is atomic
            }
        }
    }
    QVector<InputKeyBinding> clean;
    clean.reserve(keys.size());
    for (const auto &b : keys)
        if (b.key != 0) clean.append(b);
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

QString InputMap::displayText(InputAction a) const
{
    QStringList names;
    for (const auto &b : mBindings[int(a)]) {
        const QString n = keyName(b.key);
        if (!n.isEmpty()) names << n;
    }
    if (usesMouse(a))
        names.prepend(QStringLiteral("Mouse"));
    if (names.isEmpty()) return QStringLiteral("(unbound)");
    return names.join(QStringLiteral(" / "));
}

QVariantMap InputMap::toVariantMap() const
{
    QVariantMap m;
    for (int i = 0; i < kInputActionCount; ++i) {
        const InputAction a = InputAction(i);
        const bool axis = typeOf(a) == InputActionType::Axis2D;
        QStringList encoded;
        for (const auto &b : mBindings[i]) {
            const QString s = encodeKey(b, axis);
            if (!s.isEmpty()) encoded << s;
        }
        m.insert(actionName(a), encoded);
    }
    return m;
}

bool InputMap::fromVariantMap(const QVariantMap &m)
{
    bool understoodAny = false;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        InputAction a;
        if (!actionFromName(it.key(), a)) continue;   // tolerant: a future action
        const bool axis = typeOf(a) == InputActionType::Axis2D;
        QVector<InputKeyBinding> keys;
        const QStringList rows = it.value().toStringList();
        for (const QString &row : rows) {
            InputKeyBinding b;
            if (decodeKey(row, axis, b)) keys.append(b);
        }
        mBindings[int(a)] = keys;
        understoodAny = true;
    }
    return understoodAny;
}

void InputMap::load(QSettings &settings)
{
    QVariantMap m;
    for (int i = 0; i < kInputActionCount; ++i) {
        const QString key = QStringLiteral("input/") + actionName(InputAction(i));
        if (!settings.contains(key)) continue;    // no row = keep the default
        m.insert(actionName(InputAction(i)), settings.value(key).toStringList());
    }
    if (!m.isEmpty()) fromVariantMap(m);
}

void InputMap::save(QSettings &settings) const
{
    const QVariantMap m = toVariantMap();
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        settings.setValue(QStringLiteral("input/") + it.key(), it.value().toStringList());
}

void InputMap::clearPersisted(QSettings &settings)
{
    for (int i = 0; i < kInputActionCount; ++i)
        settings.remove(QStringLiteral("input/") + actionName(InputAction(i)));
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
    const bool wasHeld = mHeld.contains(key);
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
    mHeld.remove(key);
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

void InputSystem::setSettings(QSettings *settings)
{
    mSettings = settings;
    if (mSettings) mMap.load(*mSettings);
}

bool InputSystem::bind(InputAction a, const QVector<InputKeyBinding> &keys,
                       QString *conflictAction, QString *conflictKey)
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
