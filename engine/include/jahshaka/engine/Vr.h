#pragma once
// THE ENGINE's VR TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

// ---------------------------------------------------------------------------
// VR (SPECS/VR_SPEC.md v3 phase 2). The whole surface is Ogre-free AND
// OpenXR-free: nothing below names a runtime type, so a host compiled against
// this header needs no OpenXR headers and a build without the loader still
// links (every call answers "unavailable").
// ---------------------------------------------------------------------------

/// WHY A VR START DID NOT HAPPEN (VR-START-1): one value per failure a host
/// must tell a person about, each with its own sentence in VrInfo::reason.
/// VR IS NO LONGER DECIDED AT BOOT: every engine boots on its OWN Vulkan
/// device, created with the OpenXR interop extensions (the fork's interop
/// set), and each session CONNECTS through XR_KHR_vulkan_enable — a fresh
/// XrInstance per session, bound to that device. So a headset connected after
/// launch, or reconnected after a drop, is simply the next session.
enum class VrFailure {
    None = 0,
    Disabled,        ///< the host's policy forbids VR in this process (VrPolicy::enabled)
    Headless,        ///< the NULL render system has no device to share
    NoRuntime,       ///< no active OpenXR runtime manifest, or the loader found none
    WrongRuntime,    ///< the active runtime is not one the policy allows — NO OpenXR call was made
    NoHeadset,       ///< the runtime answered, no head-mounted display is connected
    RuntimeBroken,   ///< instance/system/graphics setup or session creation failed
    DeviceMismatch,  ///< the runtime drives a different GPU (or lacks an interop extension) than the engine's
    ConnectionLost   ///< the runtime or the headset went away during a session
};

/// The OpenXR session's lifecycle, as the runtime reports it
/// (XrSessionState, one for one, plus the two states that are ours).
///
/// `Unavailable` = no session exists and none can (vrAvailable() false).
/// `Lost` = the runtime or the device went away mid-session; the session is
/// ended cleanly and cannot be restarted in this process (VR_SPEC §7 item 9 —
/// at this pin a lost EXTERNAL device is unrecoverable by construction).
enum class VrState {
    Unavailable = 0,
    Idle,            ///< a session object exists, the runtime has not said Ready
    Ready,           ///< xrBeginSession has been called
    Synchronized,    ///< the runtime is consuming frames; nothing is displayed
    Visible,         ///< displayed, not focused (the dashboard is up)
    Focused,         ///< displayed and receiving input
    Stopping,        ///< xrEndSession pending
    Lost
};

/// What the desktop shows while a session runs (VrConfig::mirror).
enum class VrMirrorMode {
    None = 0,
    Left,     ///< the left eye's layer of the eye pair — the default
    Right,
    Both      ///< both eyes side by side, squeezed into the mirror's own aspect
};

/// WHAT IS ON THE DESKTOP RIGHT NOW (lane MIRROR-LIVE-1, the owner's F2 of the
/// push-#50 smoke). `VrMirrorMode` is the WISH — which half of the headset's
/// picture to copy — and this is the ANSWER for the frame that just ran:
/// either the eye's copy (one render pipeline, the desktop View switched off)
/// or the desktop's own live camera (the runtime wants no picture, so there is
/// no eye to copy and the editor draws its own).
///
/// The rule that chooses between them is stated once, in
/// `VrSession::setDesktopShowsEye`.
enum class VrDesktopPicture {
    Own = 0,  ///< the desktop View is drawing its own camera
    Eye       ///< the desktop View is off and the mirror paints the headset's eye
};

/// What the runtime is and what it wants — filled once at boot (the identity
/// half) and completed when a session begins (the size/refresh half, which
/// needs no session on any runtime measured but is reported from one place).
struct VrInfo {
    /// The LAST connection (a session's, or Engine::vrProbe's) reached a runtime
    /// with a headset. The fields below describe that runtime and stay readable
    /// after its session has ended.
    bool        available = false;
    /// Why not, when `available` is false: the loader's or the runtime's own
    /// failure, in words, at the step it happened. Empty when available.
    std::string reason;
    VrFailure   failure = VrFailure::None;
    /// The active runtime's manifest as the loader would resolve it (path and
    /// its `runtime.name`), empty when none was found.
    std::string manifest;
    std::string manifestRuntime;
    /// How many connections this PROCESS has let reach the OpenXR loader. A
    /// start refused by the policy (WrongRuntime, or NoRuntime with no manifest)
    /// does not move it — the proof that no runtime was opened.
    unsigned    openxrCalls = 0;
    std::string runtime;        ///< XrInstanceProperties::runtimeName ("Monado(XRT) ...")
    std::string runtimeVersion; ///< "M.m.p" of XrInstanceProperties::runtimeVersion
    std::string system;         ///< XrSystemProperties::systemName ("Meta Quest Pro on WiVRn")
    /// The OpenXR version the instance was created at — negotiated, never
    /// assumed: 1.1 is asked for and 1.0 is the retry (VR_SPEC §0, the Oculus
    /// audit: Meta's PC runtime has no 1.1 conformance).
    unsigned    apiMajor = 0, apiMinor = 0;
    /// The runtime's RECOMMENDED per-eye size. Never ours to pin: Monado's
    /// null compositor says 320x240 and its xcb one 896x1007; the Quest Pro
    /// over WiVRn says 2160x2376.
    unsigned    eyeWidth = 0, eyeHeight = 0;
    /// From `predictedDisplayPeriod` of the first frame (60.0 on the simulated
    /// HMD, 90.0 on the Quest Pro over WiVRn). 0 until a session has pumped.
    float       refreshHz = 0.0f;
    /// "stage" (a FLOOR origin — what a scene authored with the ground at
    /// y = 0 needs) or "local" (the runtime offers no stage; the head sits
    /// where the wearer was when the session began). Phase 1b measured why
    /// this matters: in LOCAL the Quest Pro's eyes were at y = -0.70 m, under
    /// the floor. Empty until a session has been created.
    std::string space;
    bool        visibilityMask = false;  ///< XR_KHR_visibility_mask is advertised
    bool        depthLayer = false;      ///< XR_KHR_composition_layer_depth is advertised
};

// (MOVED UP FROM BESIDE `VrStatus` BY STAGE 3, VR-HANDS-1: a profile path is now
//  a field of a HAND — `VrHandState::profile` — because the runtime binds one per
//  hand and a wearer may hold a controller in one and nothing in the other, so the
//  type has to be declared before the hand is. Nothing about it changed.)
/// AN INTERACTION PROFILE'S PATH, WITHOUT A HEAP (VR-INPUT-1E-FIX finding 8).
///
/// `VrStatus` is COPIED several times per frame — every host reads it by value
/// (`const VrStatus st = engine->vrStatus()`), the mirror keeps its own copy,
/// and at ninety frames a second that was three or four small allocations a
/// frame for one string nobody edits. A fixed array makes the whole status
/// trivially copyable, which is what a per-frame value type should be.
///
/// SIXTY-FOUR BYTES IS THE WHOLE OPENXR NAMESPACE with room to spare: the
/// longest profile path any runtime can bind here is
/// `/interaction_profiles/microsoft/motion_controller` (49) and the registry's
/// longest is 57. A longer one is TRUNCATED rather than dropped — the string is
/// diagnostic and a prefix still names the vendor — and always terminated.
///
/// It converts to `std::string` implicitly so the one place that hands the path
/// out of the engine (the `vr.state()` verb) is unchanged: the allocation
/// happens THERE, once per call, instead of on every frame's copy.
struct VrProfileName {
    char text[64] = { 0 };
    bool empty() const { return text[0] == '\0'; }
    const char *c_str() const { return text; }
    /// Is `needle` somewhere in the path? (Which model to draw is decided this
    /// way: a Touch controller is any path with `touch_controller` in it.)
    bool contains(const char *needle) const {
        return needle && *needle && std::strstr(text, needle) != nullptr;
    }
    /// Does the path START with `prefix`? (`/interaction_profiles/` — "the
    /// runtime bound something real".)
    bool startsWith(const char *prefix) const {
        if (!prefix) return false;
        const size_t n = std::strlen(prefix);
        return std::strncmp(text, prefix, n) == 0;
    }
    void assign(const char *s) {
        if (!s) { text[0] = '\0'; return; }
        std::strncpy(text, s, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
    }
    void clear() { text[0] = '\0'; }
    VrProfileName &operator=(const char *s) { assign(s); return *this; }
    operator std::string() const { return std::string(text); }
};

/// ONE LOCATED THING, IN WORLD SPACE — the rig already applied (phase 4).
///
/// `valid` is the runtime's own answer for THIS frame: a controller that is
/// switched off, out of the tracking volume or simply not held reports nothing,
/// and a host that drew a marker at the origin for it would be inventing a
/// hand. Unlike VrStatus::posesValid (which latches the last HEAD pose because
/// a locomotion rule must not stutter on one skipped frame) this goes false the
/// moment the runtime stops answering: a proxy that is not there must not be
/// drawn, and there is nothing to keep walking.
struct VrPose {
    bool valid = false;
    Vec3 position;
    Quat rotation;
};

/// WHICH HAND, and what indexes VrStatus::hands.
enum VrHand : unsigned { VrHandLeft = 0, VrHandRight = 1, VrHandCount = 2 };

// ---------------------------------------------------------------------------
// BARE HANDS (SPECS/VR_INPUT_SPEC.md §7, phase 4b stage 3). Everything in this
// block is a PURE RULE over numbers and a profile path — no OpenXR type, no
// engine state — so the session applies it, the host's drawer applies the same
// one, and `vr.grab_maths` asserts all of it with no runtime, no display and no
// headset (which is the only way a sign or a threshold in here is ever seen to
// be wrong before somebody is wearing it).
// ---------------------------------------------------------------------------

/// IS THIS PROFILE A HAND RATHER THAN A CONTROLLER?
///
/// Both hand profiles in the OpenXR registry carry `hand_interaction` in their
/// path — `/interaction_profiles/ext/hand_interaction_ext` (the one this engine
/// suggests) and `/interaction_profiles/microsoft/hand_interaction` (which some
/// runtimes advertise; if one ever binds it, it is still a hand and the same
/// answers are right). A controller profile never does.
///
/// WHAT IT DECIDES: the press thresholds (a pinch is not a trigger, below), the
/// MANIPULATION FRAME (a pinch point is not a palm, below), and which model a
/// host draws for that hand (the vendored controller, or the wearer's own
/// joints).
inline bool vrIsHandProfile(const char *profile) {
    return profile && *profile && std::strstr(profile, "hand_interaction") != nullptr;
}

/// THE PRESS THRESHOLDS — TWO PAIRS, BECAUSE A PINCH IS NOT A TRIGGER.
///
/// The VALUE is always the runtime's (it decides what "pinched" means from its
/// own tracking, and on hands it is a confidence rather than a travel); the
/// HYSTERESIS is ours, and it exists so that a hand resting on the threshold
/// cannot chatter a gesture on and off at ninety frames a second.
///
/// A TRIGGER travels a centimetre under a finger that is already braced against
/// it, so 0.5 up / 0.4 down is a tenth of its travel and enough.
/// A PINCH has no detent, no spring and no end stop: the wearer's fingers drift
/// through the middle of the range on the way to anything else they do, and a
/// runtime's pinch value wanders while two fingertips are merely close. So the
/// band is WIDE — 0.7 to press, 0.3 to release — which costs a pinch a little
/// more commitment and buys the wearer a hold that does not let go while they
/// move their hand. (The numbers are the spec's §7 and are the one thing in
/// this lane that only the owner's headset can judge; they live here, once.)
inline constexpr float kVrTriggerPressOn = 0.5f, kVrTriggerPressOff = 0.4f;
inline constexpr float kVrPinchPressOn = 0.7f, kVrPinchPressOff = 0.3f;

/// ONE PRESS, FROM ONE VALUE AND THE LAST ANSWER. `latch` is the previous
/// frame's press for that control; the return is this frame's.
inline bool vrPressLatched(float value, bool latch, float on, float off) {
    return latch ? (value > off) : (value >= on);
}

/// HOW MANY JOINTS A TRACKED HAND HAS (`XR_HAND_JOINT_COUNT_EXT`), and the
/// ORDER they arrive in — which is the extension's own enumeration and is
/// relied on by the bone table below:
///
///   0 palm · 1 wrist · 2-5 thumb (metacarpal, proximal, distal, tip)
///   6-10 index · 11-15 middle · 16-20 ring · 21-25 little
///   (each finger: metacarpal, proximal, intermediate, distal, tip)
enum : unsigned { kVrHandJointCount = 26 };

/// THE HAND'S SKELETON AS SEGMENTS — what a drawer draws.
///
/// TWENTY-FOUR BONES: the four finger chains (4 each), the thumb's three, and
/// the FAN from the wrist out to the five metacarpals, which is the palm. The
/// palm JOINT (0) is deliberately not in it: it is the middle of the hand, not
/// an end of anything, and it is what a controller's grip pose stands in for.
struct VrHandBone { unsigned char from = 0, to = 0; };
enum : unsigned { kVrHandBoneCount = 24 };
inline constexpr VrHandBone kVrHandBones[kVrHandBoneCount] = {
    // the palm: the wrist out to each metacarpal
    { 1, 2 }, { 1, 6 }, { 1, 11 }, { 1, 16 }, { 1, 21 },
    // the thumb (no intermediate joint)
    { 2, 3 }, { 3, 4 }, { 4, 5 },
    // index, middle, ring, little
    { 6, 7 }, { 7, 8 }, { 8, 9 }, { 9, 10 },
    { 11, 12 }, { 12, 13 }, { 13, 14 }, { 14, 15 },
    { 16, 17 }, { 17, 18 }, { 18, 19 }, { 19, 20 },
    { 21, 22 }, { 22, 23 }, { 23, 24 }, { 24, 25 },
};

/// WHERE ONE BONE'S SEGMENT GOES — the ONE definition, because there are two
/// writers (a running session, inside its own frame, and the host's mirror for
/// a frame no session drew — the same arrangement as the controller proxies).
///
/// THE GEOMETRY IS A UNIT SEGMENT DOWN -Z, exactly like the pointing ray's
/// (SceneMirror::syncVrProxies): the node is stood at `from`, turned so that
/// its own -Z runs to `to`, and scaled along Z to the distance between them. So
/// a hand that moves every frame rebuilds NO geometry at all — twenty-four
/// transforms per hand and one mesh in the whole scene — which is why this is
/// not a line strip per finger (a strip's points ARE its vertex buffer, and a
/// per-frame rebuild is a create-and-destroy of a GPU buffer ninety times a
/// second; see XID-1 for what recycled Vulkan blocks cost).
///
/// A DEGENERATE BONE (two joints at the same place, which a runtime reports
/// while a hand is half-tracked) answers false and is drawn by nobody, rather
/// than becoming a normalisation by zero.
inline bool vrBoneTransform(const Vec3 &from, const Vec3 &to,
                            Vec3 &position, Quat &rotation, Vec3 &scale) {
    const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(len > 1e-5f)) return false;
    const float ix = dx / len, iy = dy / len, iz = dz / len;
    // The shortest rotation taking -Z onto the bone's direction. `v = -Z x d`,
    // `w = 1 + (-Z . d)`, then normalise — the standard two-vector form, with
    // the antiparallel case (d == +Z, w == 0) spelled out rather than left to
    // divide by zero: any axis perpendicular to Z will do and X is one.
    // u = (0, 0, -1), v = d:  u x v = (v.y, -v.x, 0),  u . v = -v.z.
    const float dot = -iz;
    float qx = iy, qy = -ix, qz = 0.0f, qw = 1.0f + dot;
    if (qw < 1e-6f) { qx = 1.0f; qy = 0.0f; qz = 0.0f; qw = 0.0f; }
    const float n = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    position = from;
    rotation = Quat(qx / n, qy / n, qz / n, qw / n);
    scale = Vec3(1.0f, 1.0f, len);
    return true;
}

/// ONE HAND'S WHOLE INPUT, FOR ONE FRAME (SPECS/VR_INPUT_SPEC.md §2, phase 4b
/// stage 1). Two poses and the controls, in the frame a caller can reason in.
///
/// THE TWO POSES ARE DIFFERENT QUESTIONS, and both are the runtime's answers
/// rather than ours. `grip` is where the hand IS — the middle of the fist round
/// the controller, which is where a model or a marker is drawn — and `aim` is
/// where the hand POINTS: a runtime-authored ray whose origin sits forward of
/// the fist and whose direction is the controller's own pointing axis, which on
/// every headset measured is NOT the grip's -Z. A pointer built out of the grip
/// pose is a pointer that disagrees with the wearer's own hardware.
///
/// THE RAY IS -Z OF `aim.rotation`, by the OpenXR convention this whole engine
/// keeps: origin = aim.position, direction = aim.rotation * (0, 0, -1).
///
/// EVERY CONTROL IS A VALUE PLUS ITS EDGE-FREE PRESS, never a wall-clock or a
/// latch: `select`/`grab` are the analogue 0..1 (a bool input reads 0 or 1),
/// and the `*Pressed` booleans are those values through ONE threshold with
/// hysteresis (0.5 on the way up, 0.4 on the way down) so a trigger resting on
/// the line does not chatter. A host that wants an EDGE compares two frames —
/// which it must do anyway, because a frame the runtime skipped has no press in
/// it at all.
///
/// `valid` is this frame's answer for the whole hand: a controller switched off,
/// put down or out of the tracking volume reports nothing, and everything below
/// is then at its zero. Like VrPose::valid and unlike VrStatus::posesValid it
/// never latches.
struct VrHandState {
    bool   valid = false;        ///< the runtime reports this hand this frame
    VrPose aim;                  ///< world space through the rig: the pointing ray
    VrPose grip;                 ///< world space through the rig: where the model is drawn
    /// THE MANIPULATION FRAME — WHERE THE HAND HOLDS SOMETHING (stage 3,
    /// VR_INPUT_SPEC §7). It is the frame a grab is measured in: the object is
    /// captured relative to it and follows it rigidly.
    ///
    /// ON A CONTROLLER IT IS THE GRIP, and nothing changes: the wearer's fist
    /// is round the controller and the thing they pick up is in that fist.
    /// ON BARE HANDS IT IS THE PINCH POSE (`pinch_ext/pose`) — the point where
    /// the fingertips meet — because the grip of a hand is the PALM, and an
    /// object welded to the palm while the wearer pinches hangs several
    /// centimetres off the fingers that are supposed to be holding it, rotating
    /// about the wrong point every time they turn their hand.
    ///
    /// WHICH ONE IS CHOSEN BY THE BOUND PROFILE (`vrIsHandProfile(profile)`),
    /// and the choice is made ONCE, in the engine, so nothing above the
    /// boundary has to know whether the wearer is holding anything. It is never
    /// invalid while `grip` is valid: with no pinch pose to be had it IS the
    /// grip.
    VrPose manipPose;
    float  select = 0.0f;        ///< trigger 0..1
    bool   selectPressed = false;
    float  grab = 0.0f;          ///< squeeze 0..1
    bool   grabPressed = false;
    bool   menuPressed = false;
    float  stickX = 0.0f, stickY = 0.0f;   ///< -1..1
    bool   stickPressed = false;
    /// WHAT THE RUNTIME HAS BOUND FOR THIS HAND, as its own path (stage 3).
    ///
    /// PER HAND, BECAUSE THE RUNTIME BINDS PER HAND: WiVRn binds
    /// `ext/hand_interaction_ext` while the controllers are down and
    /// `oculus/touch_controller` for a hand that picks one up — each hand on
    /// its own, so a wearer really can hold a controller in one hand and
    /// nothing in the other (hands and controllers are alternative MODES on
    /// WiVRn, never simultaneous per hand: VR_INPUT_SPEC §1.5).
    ///
    /// It is what decides the press thresholds and the manipulation frame
    /// above, which model a host draws for this hand, and — because a mode
    /// change means the wearer's hand physically changed shape mid-gesture —
    /// when a gesture on this hand is CANCELLED (a host compares it with the
    /// previous frame's; the engine keeps no edge of its own).
    ///
    /// Empty = the runtime has bound nothing for this hand: no controller, bare
    /// hands the runtime does not track, or an unfocused session.
    VrProfileName profile;
    /// IS THIS HAND'S SKELETON BEING TRACKED this frame (`XR_EXT_hand_tracking`
    /// answered `isActive` and located its joints)? The joints themselves are
    /// NOT in this struct — fifty-two poses would be two kilobytes on a value
    /// every host copies several times a frame — they are fetched on demand
    /// through `Engine::vrHandJoints`.
    bool   jointsTracked = false;
    /// A TEST HOOK WROTE THIS SAMPLE (Engine::vrInjectInput), not the runtime.
    /// Reported so nothing downstream can mistake an injected gesture for a
    /// wearer's — a smoke in a headset that ever sees this true is looking at a
    /// stale injection, which is exactly what the refusal rule prevents.
    bool   fromInjection = false;
    // (THERE IS NO PER-HAND `focused` HERE — VR-INPUT-1E-FIX, the second read
    // of the Studio half. Focus is a property of the SESSION: the runtime takes
    // it away for the whole application, never for one hand, so the same bit
    // was written to both hands and every consumer had to fold two copies of
    // one truth back together — and a host that OR-ed them could miss a cancel
    // (an unfocus on one hand while a stale sample on the other still said
    // focused). It lives on `VrStatus::inputFocused` now, once.)
};

/// The engine's own `VrHandState` exists — what the Studio side's guarded
/// mirror of this struct (src/modules/vr/vrinteraction.h) compiles out
/// against, so the two halves of phase 4b stage 1 never define it twice.
#define JAH_ENGINE_HAS_VRHANDSTATE 1

/// THE CONTROLLER'S RAY AND ITS HIT, AS THE HOST COMPUTED THEM
/// (Engine::setVrRay; VR_INPUT_SPEC §3). The engine DRAWS this and nothing
/// else: the picking is the document's (one picker, `iris::picking`), the
/// selection rules are the host's, and the two helper nodes the mirror owns are
/// placed from these numbers inside the frame that draws them.
///
/// `origin` and `dir` are WORLD space; `dir` need not be normalised. With
/// `hit` the line stops at `hitPoint` and a small marker is drawn there;
/// without it the line runs to `length` (or ten metres).
struct VrRayState {
    bool visible = false;
    Vec3 origin;
    Vec3 dir;
    bool hit = false;
    Vec3 hitPoint;
    /// How far the line runs when nothing was hit, metres. 0 = ten metres.
    float length = 0.0f;
    /// WHICH HAND IT BELONGS TO (VrHandLeft/VrHandRight), or -1 for "take the
    /// origin and direction exactly as given".
    ///
    /// WHY IT IS WORTH A FIELD. A host computes this ray from the pose it last
    /// HEARD, which is a frame or two old — the same lag that made the engine
    /// place the controller proxies itself (Scene::setVrProxyNodes). Naming the
    /// hand lets the session re-anchor the line to THIS frame's aim pose while
    /// keeping the host's own length, so the ray leaves the wearer's hand
    /// exactly where the model is and only its far end is as old as the pick.
    int hand = -1;
};

/// A session's parameters. Everything here is the HOST's choice; nothing is
/// persisted by the engine.
struct VrConfig {
    VrMirrorMode mirror = VrMirrorMode::Left;
    /// Metres of world per metre of room. 1 = life size.
    float worldScale = 1.0f;
    /// FOR MEASUREMENT ONLY (VR_SPEC §5 phase 2's "at 2160x2376 per eye"): render
    /// each eye at this size instead of the runtime's recommendation. The XR
    /// swapchains still use the runtime's size, so the copy scales — which is
    /// exactly what makes it a measurement of the RENDER and not of the runtime.
    /// 0 = use the runtime's recommendation (the product path).
    unsigned overrideEyeWidth = 0, overrideEyeHeight = 0;
    /// HOW MANY STEREO WARM-UP FRAMES THE SESSION RENDERS BEFORE ITS FIRST
    /// COMMITTED EYE FRAME (lane VR-WARMUP-1). 0 = none.
    ///
    /// THE MEASUREMENT THIS EXISTS FOR (spikes/vr-warmup-1): the session's
    /// SECOND frame — the first one the runtime asks a picture of — cost
    /// 1,179 ms cold and 89 ms warm on the Grand Showroom (383 / 20 on the
    /// default scene), all of it inside `engine.record`'s two `Jahshaka opaque`
    /// passes with the GPU idle: Hlms permutations generated, shaders compiled
    /// and pipelines built ON THE FRAME THREAD, at the moment the wearer is
    /// first shown the world. At 62.5 Hz that is 73 repeated headset frames.
    ///
    /// WHY IT CANNOT BE PAID ANYWHERE ELSE. The desktop's own warm-up does not
    /// cover it (measured: 783 ms on the second VR frame after 200 mono frames
    /// in the same process) because the INSTANCED-STEREO permutation set is its
    /// own — `hlms_instanced_stereo` is a pass property, so every shader the
    /// eyes need is a different shader from the one the desktop compiled; and
    /// a fully warm microcode cache still paid 498 ms when the EYE SIZE changed
    /// (two permutations whose generated source depends on the target). So the
    /// warm-up renders the SESSION's own chain, stereo, at the SESSION's eye
    /// size: the only shape that covers both.
    ///
    /// WHAT A WARM-UP FRAME IS: a real frame of the session's view, from the
    /// rig's origin, through a deliberately wide frustum (so culling keeps
    /// nothing back), with NO XR frame open — nothing is submitted to the
    /// runtime and nothing is mirrored. Frame k faces the origin's heading
    /// turned by k*180 degrees, so two frames see the whole room.
    ///
    /// TWO by default: one forward, one back. The cost moves to the start of
    /// the session, where the runtime is still showing its own picture and
    /// asking for none (VrStatus::warmUpMs says what it cost).
    unsigned warmUpFrames = 2;
    /// DOES THE WEARER SEE THE EDITOR'S FURNITURE — the ground grid, the light
    /// and camera icons, the selection outline, the gizmo (owner, 2026-09-17)?
    ///
    /// IT FOLLOWS THE HOST MODE, and the host is the only one who knows which
    /// it is. The EDITOR's VR preview passes true: the whole point of that mode
    /// is to stand inside the scene and watch the editor work, and a wearer who
    /// cannot see what is selected or where the grid is cannot author. The
    /// PLAYER passes nothing and gets this default: a play session shows no
    /// furniture, exactly as the desktop Player shows none.
    ///
    /// The VR CONTROLLER proxies are NOT governed by it — they carry their own
    /// channel (kVrHelperBit) and are drawn in every VR eye, both modes, like
    /// Unreal's. Off by default here, so a host that says nothing gets the
    /// conservative answer.
    bool helpers = false;
    /// THE REFLECTION ROW THE HEADSET RENDERS WITH (lane REFLECT-VR-1), the
    /// same 0/1/2 as `PostFxDesc::ssr` — 0 off, 1 half-resolution, 2 full —
    /// and since lane EYE-GRADE-1 an OVERRIDE rather than the value itself:
    /// **-1, the default, means "whatever the project's World panel says"**.
    ///
    /// IT USED TO BE THE HOST'S TO PASS because the session's View was the one
    /// view no mirror reached, so the project's row had no other channel. That
    /// is no longer true — `SceneMirror::buildViewEnvironment` pushes the whole
    /// of the project's post description into the session's view every frame —
    /// so a host that says nothing gets the author's row by the ordinary route,
    /// and this field is what `vr.begin({reflections:n})` sets: one session,
    /// one measurement, nothing written to the project.
    ///
    /// The SOURCE is not a choice here: a stereo chain never marches in screen
    /// space (see `PostFxDesc::ssr`), so this row buys RAY-TRACED
    /// reflections on a ray-capable machine and NOTHING AT ALL on one without —
    /// `chain::build` declines to build the reflection stage when neither source
    /// can write it, so a machine with no ray queries does not even pay the
    /// prepass and renders exactly what it renders today.
    int ssr = -1;
    /// THE RUNTIME'S HIDDEN-AREA MESH (lane HAM-1): mask out the corners of
    /// each eye that the headset's own lenses never show, so no shading is
    /// spent on them.
    ///
    /// ON BY DEFAULT and there is no host row for it: it costs nothing a wearer
    /// can see (the pixels it removes are behind the lens barrel) and it is
    /// bought from the runtime's own geometry (`XR_KHR_visibility_mask`), so a
    /// runtime that answers no mask simply renders what it rendered before.
    /// The flag exists so a MEASUREMENT can take the other arm in the same
    /// process — the suite's mask-off control and the lane's own A/B — which is
    /// the same reason `overrideEyeWidth` is here.
    bool hiddenAreaMask = true;
    /// DOES THIS SESSION BIND THE WEARER'S BARE HANDS (lane HANDS-SWITCH-1;
    /// the owner, 2026-09-18, joint: bare-hand work is deferred until the
    /// controllers are right, and hands-or-controllers is the AUTHOR'S choice
    /// per project rather than the runtime's per moment)?
    ///
    /// OFF BY DEFAULT, and off means the session has no bare-hand route at all:
    ///   * the `ext/hand_interaction_ext` suggested-binding block is NOT
    ///     offered, so the runtime can never bind a hand profile for either
    ///     hand — a wearer who puts a controller down is left holding nothing,
    ///     which is what "controllers only" has to mean;
    ///   * no `XrHandTrackerEXT` is created, so nothing asks the runtime for
    ///     joints; and
    ///   * no skeleton is reported or drawn, the test-injection route included
    ///     (`Engine::vrHandJoints` answers 0 while such a session is live).
    /// The controllers are untouched by it: the other three binding blocks are
    /// offered exactly as before.
    ///
    /// THE HOST PASSES THE PROJECT'S OWN ROW (`iris::Scene::vrHands`, the World
    /// panel's Hands switch and `world.vr({hands})`); `vr.begin({hands})`
    /// overrides it for ONE session, which is what a suite and a measurement
    /// need, exactly like `hiddenAreaMask` above.
    ///
    /// IT IS READ ONCE, at session creation: suggested bindings are attached to
    /// the session's action sets before its first frame and no runtime can be
    /// asked to rebind them, so there is nothing here for a mid-session change
    /// to act on.
    bool hands = false;
};


/// ONE SUGGESTED-BINDING BLOCK, AS THE RUNTIME ANSWERED IT (stage 3's fix
/// round; `Engine::vrBindingBlocks`).
///
/// WHY PER PROFILE AND NOT JUST A PAIR OF TOTALS. `bindingProfiles` /
/// `bindingProfilesAccepted` say "four offered, four taken", which cannot
/// distinguish a block that bound every path it meant to from one that bound
/// half of them: a path spelled wrong, or a profile that lost an input between
/// pin bumps, takes that hardware's control away SILENTLY and the totals still
/// read 4 of 4. The COUNT is what pins it — the bare-hand block is twelve
/// bindings (two poses, the pinch pose, select, grab, per hand) and a suite
/// asserts that number.
///
/// `bindings` is how many `XrActionSuggestedBinding`s the block carried;
/// `accepted` is whether `xrSuggestInteractionProfileBindings` took it (a
/// runtime refuses a profile it does not know, which is not an error).
struct VrBindingBlock {
    VrProfileName profile;
    unsigned      bindings = 0u;
    bool          accepted = false;
};
/// How many blocks this engine can report (it offers four).
enum : unsigned { kVrBindingBlockMax = 8 };

/// What a live session is doing. Every number is a COUNT or a measured value,
/// never a wall-clock derivation (VR_SPEC §6 flake class (b): count frames,
/// never time, on a loaded box).
struct VrStatus {
    VrState            state = VrState::Unavailable;
    bool               active = false;      ///< a session object exists
    unsigned long long frames = 0;          ///< xrEndFrame calls that succeeded
    unsigned long long rendered = 0;        ///< frames the runtime asked us to draw
    /// VR frames begun OFF the engine's render thread (ENGINE-UPGRADE-U3 stage 6, the XR-thread law: xrWaitFrame /
    /// xrBeginFrame / xrEndFrame run on the render thread — 0 always).
    unsigned long long framesOffRenderThread = 0;
    /// THE RUNTIME'S CLOCK (ENGINE-UPGRADE-U3 stage 6): the newest frame's predictedDisplayTime, ns, on the runtime's
    /// own timeline (0 before the first) — what the wearer's interaction steps by (the delta between two reads),
    /// never a wall clock of ours.
    long long          displayTimeNs = 0;
    /// The runtime's display rate (predictedDisplayPeriod; VrInfo::refreshHz) — 0 until a session has pumped. What the
    /// host's loop sends as the display period while the session runs (U3 decision 53), never the panel's.
    float              refreshHz = 0.0f;
    /// DISPLAY PERIODS THE APPLICATION MISSED (ENGINE-UPGRADE-U3, vr.ui_stall): over every xrWaitFrame after the
    /// session's first, the runtime's own predicted display times stepped by more than one predicted display period
    /// — a COUNT of the headset refreshes no frame of ours was submitted for, read from the runtime's clock, never
    /// from ours. 0 on a session that kept up with its runtime.
    unsigned long long missedDisplayPeriods = 0;
    /// The distance between the two located eye positions, metres. 0 before the
    /// session's first frame; through the WARM-UP frames it reads the synthetic
    /// pair's 0.064 (VrConfig::warmUpFrames) until the first real xrLocateViews
    /// — `head.valid` is false for as long as that is so.
    float              ipd = 0.0f;
    unsigned           eyeWidth = 0, eyeHeight = 0;   ///< what the chain renders per eye
    /// THE COLOUR CONTRACT THIS SESSION IS UNDER (lane EYE-GRADE-1), reported
    /// rather than only logged because it decides whether the wearer is looking
    /// at the project's picture or at a picture a stop too bright.
    ///
    /// `swapchainFormat` is the Vulkan format the runtime gave us, by name
    /// ("R8G8B8A8_SRGB"), and `colourEncodedOnce` is what it MEANS: the eye
    /// target's bytes are display-encoded, so an _SRGB swapchain has the
    /// runtime decode and re-encode them (an identity round trip, one encode
    /// from radiance to the eye) while a UNORM one has it treat them as linear
    /// and encode them a SECOND time. The second is a fallback for a runtime
    /// that offers nothing better — loud in the log, visible here, and raised
    /// to the user as a scene issue — and no runtime seen so far needs it.
    ///
    /// Empty / true with no session.
    std::string        swapchainFormat;
    bool               colourEncodedOnce = true;
    /// THE STEREO WARM-UP (VrConfig::warmUpFrames): how many warm-up frames
    /// this session has rendered, and what they cost in total. `warmUpFrames`
    /// reaching the configured number is the signal that the eyes are being
    /// drawn with everything already built; a caller that reads it as 0 on a
    /// session configured for 2 is looking at a session whose runtime has not
    /// started running yet.
    unsigned           warmUpFrames = 0;
    float              warmUpMs = 0.0f;
    VrMirrorMode       mirror = VrMirrorMode::None;
    /// AND WHICH OF THE TWO PICTURES IS ACTUALLY ON THE DESKTOP (lane
    /// MIRROR-LIVE-1). `mirror` above is the host's wish for the whole session;
    /// this is what the last frame did with it, and it moves within one frame
    /// of the runtime starting or stopping to ask for pictures. `Own` with a
    /// mirror mode set means the runtime is not rendering (the headset is off
    /// the head, the dashboard is up, tracking is gone) and the desktop has
    /// taken its own camera back — which is what a person looking at the screen
    /// at that moment needs to see.
    VrDesktopPicture   mirrorShowing = VrDesktopPicture::Own;
    float              worldScale = 1.0f;
    /// Whether the per-eye PROJECTIONS differ, i.e. whether the runtime gave
    /// the two eyes different fovs. Monado's simulated HMD does not (both eyes
    /// get one symmetric fov and only the POSES separate them); the Quest Pro
    /// does. A test that asserts "the eyes differ" has to know which it has.
    bool               asymmetricFov = false;

    // ---- THE RIG IN THE WORLD (phase 3, the Player's VR mode) -------------
    /// WHERE THE HEAD IS, IN WORLD SPACE — the rig's origin composed with the
    /// pose the runtime reported, which is the only pose a host can reason
    /// about. A host that moves the wearer (locomotion) needs both halves:
    /// where it put the rig, and where the wearer's head ended up inside it.
    ///
    /// `headRotation` is the FULL head orientation (pitch and roll included);
    /// a locomotion rule that wants a level heading takes the forward vector
    /// and flattens it rather than decomposing Euler angles, which is what the
    /// Player's head-relative fly does.
    Vec3               headPosition;
    Quat               headRotation;
    /// Has the runtime EVER located the views in this session? False until the
    /// first located frame, when the two fields above hold nothing but their
    /// defaults. It deliberately does NOT go false again on a frame the runtime
    /// skips or a moment of lost tracking: the fields then hold the last pose
    /// that WAS located, which is the right thing for a host moving a wearer
    /// who is mid-stride — a locomotion rule that stopped dead on one skipped
    /// frame would stutter, and one that reset to the origin would teleport.
    bool               posesValid = false;
    /// THE RIG'S ORIGIN, AS THE HOST LAST SET IT (Engine::setVrOrigin): the
    /// world position of the reference space's origin (the floor spot the
    /// wearer stands on, in STAGE) and its heading in DEGREES about +Y.
    /// Reported back so a caller can see that the engine took what it was
    /// given rather than trusting its own copy.
    Vec3               origin;
    float              originYaw = 0.0f;
    /// THE HOST'S LAST setVrOrigin THIS ORIGIN CARRIES (ENGINE-UPGRADE-U3 stage 6): the host composes its next step
    /// from the origin it reads; while its own newer write has not landed (its sequence ahead of this one) the host's
    /// read reports that write — two steps inside one frame never compose from the same stale origin.
    unsigned long long originSeq = 0;
    /// HOW MANY TIMES THE RUNTIME RECENTRED THE ROOM under this session (the
    /// Quest's long-press, a guardian re-setup). Each one moves every pose the
    /// runtime reports discontinuously; the session ABSORBS it into the rig so
    /// the wearer stays where they were standing, and this counts them — a
    /// host that sees it climbing while nobody touched the headset is looking
    /// at a runtime problem, not at its own locomotion.
    unsigned long long spaceChanges = 0;

    // ---- THE HIDDEN-AREA MESH (lane HAM-1) -------------------------------
    /// WHERE THE MASK CAME FROM: "runtime" (the eye's own geometry through
    /// `XR_KHR_visibility_mask`), "off" (the host asked for none —
    /// `VrConfig::hiddenAreaMask` false) or "none" (the runtime has no mask to
    /// give, which is what Monado's simulated HMD used to be read as and what
    /// every runtime without the extension is). Empty with no session.
    std::string        hiddenAreaSource;
    /// THE FRACTION OF EACH EYE THE MASK COVERS, indexed by eye (0 = left),
    /// measured on the geometry the runtime handed over: the sum of its
    /// triangles' areas in the eye's own clip rectangle, which has area 4. It
    /// is the number the saving is computed from (the pixels never shaded), and
    /// it is the RUNTIME'S answer — a Quest Pro and a simulated HMD do not
    /// report the same shape, so a measurement that does not carry this number
    /// cannot be read on another headset.
    float              hiddenAreaFraction[2] = { 0.0f, 0.0f };
    /// ...and how many triangles that was, per eye. 0 = no mask on that eye.
    unsigned           hiddenAreaTriangles[2] = { 0u, 0u };

    // ---- THE HANDS (phase 4) ---------------------------------------------
    /// WHERE THE WEARER'S HANDS ARE, in world space, through the rig exactly
    /// like the head. Indexed by VrHand. POSES ONLY: no buttons, no triggers,
    /// no input actions — those are their own spec, and nothing here reads a
    /// value a user pressed.
    ///
    /// The source is the OpenXR ACTION system (a grip pose on the simple
    /// controller profile, which every runtime maps from whatever the wearer is
    /// actually holding), or the hand-tracking extension's palm joint where the
    /// runtime offers it and no controller answers. A runtime with neither
    /// leaves both invalid for the life of the session, which is not an error:
    /// a headset with no controllers is a supported headset.
    VrPose             hands[VrHandCount];
    /// EVERYTHING EACH HAND IS DOING (phase 4b stage 1, VR_INPUT_SPEC §2): the
    /// two poses and the controls. `input[i].grip` IS `hands[i]` — the same
    /// pose, reported twice because `hands` is what phase 4's hosts read and
    /// the pair is what a gesture needs.
    VrHandState        input[VrHandCount];
    /// DOES THE APPLICATION HAVE INPUT FOCUS? (VR_INPUT_SPEC §5.5; the shape is
    /// VR-INPUT-1E-FIX's — it used to be a bit on every hand.)
    ///
    /// The runtime takes focus away whenever its own dashboard comes up, the
    /// headset comes off the head or another application is talking to the
    /// wearer, and it takes it away for the WHOLE APPLICATION: `xrSyncActions`
    /// answers XR_SESSION_NOT_FOCUSED (a SUCCESS code), every action goes
    /// inactive and every control reads its zero. A host that read that as "the
    /// trigger was released" would COMMIT a gesture the wearer never finished,
    /// so the distinction is reported and a gesture in flight is CANCELLED
    /// rather than committed when this goes false.
    ///
    /// True exactly while the session is FOCUSED. With NO session it is false
    /// (nobody is holding anything) — unless a hand is INJECTED, and then it is
    /// what `Engine::vrInjectFocus` was last given, true by default: a test
    /// that says nothing about focus means "the wearer was there", and one that
    /// injects it false is driving the cancel with no dashboard to raise.
    bool               inputFocused = false;
    /// THE INTERACTION PROFILE THE RUNTIME HAS BOUND, as its own path
    /// (`/interaction_profiles/oculus/touch_controller`), or empty when it has
    /// bound none — which is what a wearer with no controllers, and every
    /// unfocused session, reports.
    ///
    /// It is the runtime's CHOICE out of the profiles we suggested.
    ///
    /// A SUMMARY, DERIVED, NOT A SECOND TRUTH (stage 3): the per-hand answer is
    /// `input[h].profile` and this is the right hand's when it has one, else
    /// the left's — because from stage 3 on the two hands really can differ (a
    /// controller in one hand, bare fingers in the other), and anything that
    /// draws a model or measures a press asks the HAND. This field stays for
    /// what it is good for: naming the session's input in a log, a report or
    /// `vr.state().profile`.
    ///
    /// A FIXED ARRAY, not a `std::string` (VrProfileName's note): this struct
    /// is copied several times per frame and a status copy allocates nothing.
    VrProfileName      profile;
    /// HOW MANY SUGGESTED-BINDING BLOCKS WERE OFFERED, and how many the runtime
    /// ACCEPTED (`xrSuggestInteractionProfileBindings`). A runtime refuses a
    /// profile it does not know (a path it cannot resolve, an extension it does
    /// not have) and that is not an error — but a build whose four blocks all
    /// failed would silently have no input at all, so the counts are reported.
    unsigned           bindingProfiles = 0, bindingProfilesAccepted = 0;
    /// The action set was created, bound and ATTACHED to this session — i.e.
    /// the controller route is live and `hands` can become valid. False means
    /// the runtime refused the actions (or the build has none), which is worth
    /// telling apart from "the controllers are switched off".
    bool               handActions = false;
    /// XR_EXT_hand_tracking is advertised, supported by the system and in use.
    /// It is a FALLBACK, never a replacement: a hand holding a controller is
    /// located by the controller.
    bool               handJoints = false;
    /// WAS THIS SESSION ASKED TO BIND BARE HANDS (`VrConfig::hands`, lane
    /// HANDS-SWITCH-1)? Reported because it is the one thing that explains the
    /// two numbers above being three-of-three rather than four-of-four, and
    /// because "is this project on hands or on controllers" is a question the
    /// owner asks of a running session — `vr.state().hands.enabled`.
    bool               handsEnabled = false;
};

}}  // namespace jahshaka::engine
