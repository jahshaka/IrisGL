/**************************************************************************
This file is part of JahshakaVR, VR Authoring Toolkit
http://www.jahshaka.com
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_INPUTMAP_H
#define IRIS_INPUTMAP_H

// InputMap / InputState / InputSystem — the gameplay action layer
// (AVATAR_LOCOMOTION_SPEC §8.2, Stage 1).
//
// FOUR named actions and no more: Move (Axis2D), Look (Axis2D), Jump (bool,
// LATCHED), Sprint (bool, HELD). Deliberately NOT Unreal's Enhanced Input:
// there are no triggers, no modifiers, no contexts with priorities and no
// user-defined actions. One binding table, one flat state struct.
//
// WHY THIS IS DOCUMENT-SIDE (irisgl, not src/): every locomotion test in this
// program has to drive input with no window, no display and no synthetic key
// events. `avatar.input({...})` writes the same struct the keyboard producer
// writes, so a headless run and a live run exercise one code path.
//
// WHY A SINGLETON: there is exactly one local player (§8.4 — one possession
// slot per scene, no player index, no split screen). The precedent in this
// tree is the same shape, KeyboardState (src/viewport/keyboardstate.h). The
// per-avatar input struct that Stage 2's movement component reads is NOT this
// object: possession (Stage 3) copies the possessed avatar's slice out of here,
// so nothing in irisgl/document/physics ever reaches for the singleton.
//
// The ShortcutRegistry is NOT this class and never will be: that one is an
// editor COMMAND registry (one QShortcut per row, QKeySequence-shaped,
// window-scoped). Gameplay keys are held, combined and polled — they appear
// in Preferences as read-only `addFixed` rows only so they are discoverable.

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

class QSettings;

namespace iris {

/// The closed action set. Adding a fifth is a spec change, not a code change.
enum class InputAction { Move = 0, Look = 1, Jump = 2, Sprint = 3 };
static constexpr int kInputActionCount = 4;

enum class InputActionType { Axis2D, Button };

/// One key's contribution to an action while it is held. For an Axis2D action
/// the held keys' (x, y) are summed and the result clamped to the unit disc
/// (§5: `moveInput` is "clamped to the unit disc"); for a Button action only
/// the key matters.
struct InputKeyBinding
{
    int   key = 0;      ///< a Qt::Key value (int, so this header needs no QtGui)
    float x   = 0.f;
    float y   = 0.f;

    bool operator==(const InputKeyBinding &o) const
    { return key == o.key && x == o.x && y == o.y; }
};

struct InputAxis2D
{
    float x = 0.f;
    float y = 0.f;
    bool isZero() const { return x == 0.f && y == 0.f; }
};

/// The flat per-frame state. `jump` is a LATCH (§5: "Latched by the input
/// layer, consumed by the movement component in the step that applies the
/// impulse. Cleared unconditionally after consumption so a held key cannot
/// re-fire"); `sprint` is a plain held bool; `move` and `look` are continuous.
struct InputState
{
    InputAxis2D move;     ///< camera-relative intent is applied by possession, not here
    InputAxis2D look;     ///< accumulated mouse delta; consumeLook() drains it
    bool sprint = false;  ///< HELD — true for as long as the key is down
    bool jump   = false;  ///< LATCH — set on the press edge, cleared by consumeJump()

    void clear() { move = InputAxis2D(); look = InputAxis2D(); sprint = false; jump = false; }
};

/// action -> keys. Pure data: no Qt widgets, no engine, no scene.
class InputMap
{
public:
    InputMap();   ///< starts at the §8.2 defaults

    // ---- action metadata (all static: the set is closed) -------------------
    static InputActionType typeOf(InputAction a);
    /// True for Jump only — the one action whose value is a one-shot latch.
    static bool isLatched(InputAction a);
    /// True for Look only — its primary producer is the mouse, so an empty key
    /// list is CORRECT rather than "unbound".
    static bool usesMouse(InputAction a);
    static QString actionName(InputAction a);              ///< "Move" / "Look" / "Jump" / "Sprint"
    static bool actionFromName(const QString &name, InputAction &out);  ///< case-insensitive
    static QVector<InputAction> allActions();

    // ---- key names --------------------------------------------------------
    /// "W", "Space", "Shift", … Round-trips with keyFromName. Uses
    /// QKeySequence's portable text for ordinary keys and an explicit table
    /// for the bare modifiers (QKeySequence spells Qt::Key_Shift "Shift+").
    static QString keyName(int key);
    /// 0 when `name` names no key.
    static int keyFromName(const QString &name);

    // ---- bindings ---------------------------------------------------------
    const QVector<InputKeyBinding> &bindings(InputAction a) const;
    /// Replaces the WHOLE key list for `a`. Refused (false, nothing written)
    /// when one of `keys` is already bound to a DIFFERENT action — the
    /// out-params then name the offender. Rebinding a key inside the same
    /// action is not a conflict, and neither is a duplicate inside `keys`
    /// (W = forward twice is silly, not wrong).
    bool bind(InputAction a, const QVector<InputKeyBinding> &keys,
              QString *conflictAction = nullptr, QString *conflictKey = nullptr);
    /// Clears every key on `a` (Look keeps working: it is mouse-driven).
    void unbind(InputAction a);

    /// True when `key` contributes to ANY action. THIS is the set the play-mode
    /// ShortcutOverride fix (§8.3) withholds from the window shortcuts.
    bool isBound(int key) const;
    /// The action `key` belongs to; `found` is false when it belongs to none.
    InputAction actionForKey(int key, bool *found = nullptr) const;

    void resetToDefaults();
    static InputMap defaults();

    /// A human display string for one action ("W / S / A / D", "Mouse",
    /// "Space") — what the Preferences fixed rows show.
    QString displayText(InputAction a) const;

    // ---- persistence ------------------------------------------------------
    /// { "Move": ["W:0,1", "S:0,-1", …], "Jump": ["Space"], … }. Axis keys
    /// carry their contribution; button keys are the bare name.
    QVariantMap toVariantMap() const;
    /// Tolerant: an unknown action name or an unparseable key is SKIPPED, not
    /// an error — a settings file written by a future build must not brick the
    /// input layer. Returns false when nothing at all was understood.
    bool fromVariantMap(const QVariantMap &m);

    /// jahsettings.ini "input/<Action>" — the same shape ShortcutRegistry uses
    /// for "shortcut/<id>". An action with no stored row keeps its default.
    void load(QSettings &settings);
    void save(QSettings &settings) const;
    /// Removes every stored row. Back-to-defaults must leave NO override keys
    /// behind — the same contract ShortcutRegistry::resetAll has, and the
    /// reason a test that rebinds can restore the shared settings file it ran
    /// against byte-for-byte.
    static void clearPersisted(QSettings &settings);

private:
    QVector<InputKeyBinding> mBindings[kInputActionCount];
};

/// The one process-wide input state plus the producers that write it.
class InputSystem
{
public:
    static InputSystem &instance();

    InputMap &map() { return mMap; }
    const InputMap &map() const { return mMap; }
    const InputState &state() const { return mState; }

    // ---- keyboard producer ------------------------------------------------
    // Callers must drop auto-repeat before calling: an auto-repeat PRESS would
    // re-latch Jump under a held key, which is the exact thing §5 forbids.
    /// Returns true when the key was bound (i.e. it was consumed by gameplay).
    bool keyPressed(int key);
    bool keyReleased(int key);
    /// Adds a mouse delta to Look. Drained by consumeLook().
    void mouseMoved(float dx, float dy);
    /// Drops the held-key set and zeroes the state. Called on focus loss and on
    /// leaving play mode — without it a key released while another widget had
    /// focus sticks down forever.
    void clearKeys();

    // ---- consumers --------------------------------------------------------
    /// Reads and clears the Jump latch. A held key cannot re-fire: the latch is
    /// only set again by a fresh press edge.
    bool consumeJump();
    /// Reads and zeroes the accumulated Look delta.
    InputAxis2D consumeLook();

    // ---- scripted producer (avatar.input) ---------------------------------
    /// Writes the state directly — the headless harness. Each field is
    /// optional; an absent field is left alone. `jump` true SETS the latch (it
    /// never clears it: clearing is consumeJump's job, so a script cannot
    /// silently swallow a pending jump).
    void setMove(float x, float y);
    void setLook(float x, float y);
    void setSprint(bool on);
    void requestJump();

    // ---- bindings + persistence ------------------------------------------
    /// jahsettings.ini, wired by the shell. Null in headless runs (no
    /// persistence, everything else works). Loads immediately when set.
    void setSettings(QSettings *settings);
    /// map().bind() plus a save(). Returns false, changing nothing, on conflict.
    bool bind(InputAction a, const QVector<InputKeyBinding> &keys,
              QString *conflictAction = nullptr, QString *conflictKey = nullptr);
    void resetBindings();

    /// Test seam ONLY: back to defaults, no held keys, empty state, settings
    /// detached. Never call this from app code.
    void resetForTest();

private:
    InputSystem() = default;
    /// Recomputes the CONTINUOUS fields (move, sprint) from the held-key set.
    /// This deliberately overwrites whatever a script wrote — the last producer
    /// to act wins, and with no key events (headless) the script's values stand.
    void recomputeHeld();

    InputMap  mMap;
    InputState mState;
    QSet<int> mHeld;
    QSettings *mSettings = nullptr;
};

/// THE §8.3 PREDICATE, in one place because two widgets have to agree on it.
///
/// True when `key` must be withheld from Qt's window-shortcut system because
/// gameplay owns it right now. `playing` is the caller's play flag — the editor
/// viewport's `mPlaying` (what IEditorViewport::isPlaying() and
/// `editor.playing()` report) and the player page's `isScenePlaying()`.
///
/// Why this is a function and not four lines copied twice: `tool.translate` is
/// on W and `tool.cycle` is on Space, both Qt::WindowShortcut, so a viewport
/// that gets this wrong silently changes the gizmo mode instead of walking —
/// and the two viewports drifting apart is exactly how the player page ended up
/// with no `event()` override at all.
bool gameplayClaimsKey(bool playing, int key);

} // namespace iris

#endif // IRIS_INPUTMAP_H
