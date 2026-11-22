#pragma once
// THE ENGINE's FRAMEINPUT TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "jahshaka/engine/Camera.h"
#include "jahshaka/engine/Fog.h"
#include "jahshaka/engine/GiModes.h"
#include "jahshaka/engine/GiParams.h"
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Lights.h"
#include "jahshaka/engine/Math.h"
#include "jahshaka/engine/Monitor.h"
#include "jahshaka/engine/Overlay.h"
#include "jahshaka/engine/Planar.h"
#include "jahshaka/engine/PostFx.h"
#include "jahshaka/engine/Sky.h"

namespace jahshaka { namespace engine {

/// WHAT A WORLD STILL OWES WHILE IT STREAMS IN (SPECS/OPEN_COVER_SPEC.md §2.1,
/// §3, lane OPEN-COVER-2b). The reading behind the loading indicator — one line
/// at the bottom of the viewport while a world fills in — and behind
/// `editor.viewportState().pending`.
///
/// CHEAP BY CONTRACT, because the host reads it once per frame while a world
/// arrives: every field is a counter or a flag already kept for another reason.
/// In particular it is NOT `shaderCacheStats()`, which stats every file in the
/// shader-cache directory to report its size — fine once, not once a frame.
///
/// HONEST ABOUT WHAT CANNOT BE COUNTED: a PSO is generated when a renderable is
/// first DRAWN, so "shaders still to compile" is not a number anything can
/// know. `shadersCompiled` is the running total for this process, and a host
/// that wants "is it still compiling" differences it across frames.
class Scene;
class View;
class Engine;

/// ONE FRAME'S INPUT (ENGINE-UPGRADE-1 U0, part 3a UPGRADE-RECORD-1). Everything a host hands the engine FOR A FRAME —
/// the frame's clock step, why it renders, its pace, the gesture, each scene's environment and CHANGE SET, and each
/// view's camera and post chain — goes in one of these through Engine::submitFrame, and the
/// engine applies it ONCE, at the top of the next renderOneFrame (the DRAIN), before anything of that frame runs.
/// Nothing a frame depends on is written into the engine on the spot any more: a value lands in the frame that
/// renders it, in one place, in one order.
///
/// EVERY FIELD IS OPTIONAL: an absent field leaves the engine's value as it is. Two submits before one frame MERGE, field by field: every field the
/// newer record carries wins and one it does not carry keeps the older value — for the frame, each VIEW and each SCENE
/// (FrameInputStatus::scenePartsMerged counts a scene part merged into an earlier one).
/// At the drain each field is COMPARED with what the engine last applied from a record and applied only when it
/// differs — so a still document drains to nothing (FrameInputStatus::fieldsApplied == 0). The per-frame field
/// (`cause`) resets after the drain to its default (Driver); the pace is the driver's setter (Engine::setFramePace, U3).
/// THE ENGINE HAS NO CLOCK OF ITS OWN: a SCENE's time is its document's grid, carried by its part's `stepSerial`
/// (FrameInputScene, ONE-FRAME-1) — a frame advances each scene by the steps its serial moved since the last, once,
/// and 0 when nobody stepped it.
/// THE FRAME'S CHANGE SET (U0 3b UPGRADE-CHANGESET-1): what the DOCUMENT wrote into the shared scene graph since the
/// scene's last record, by engine node — the one source of "what moved" the engine has. A document node's transform
/// is written straight into the Ogre node the engine renders (one hierarchy, until U2), so no engine verb runs when a
/// box is dragged, carried by a parent or animated; the mirror drains the document's write journal at its emit and
/// names the written nodes here. The engine's own writes (setNodeTransform, a rider, a decal's box, an Item
/// arriving) need no entry: each names itself at its seam.
///
/// `moved` lists the nodes whose world transform was WRITTEN; each implies its whole subtree (a moved parent moves
/// every child under it). The engine re-reads exactly those items — the GI movement scan, the caster walk, the
/// geometry signature — and bumps the light serial once when a light is among them (the re-inject). An empty set
/// costs nothing anywhere; `complete == false` (the journal could not name every write) costs one full pass.
/// ONE DOCUMENT NODE'S RECORD (ENGINE-UPGRADE-U2, the extract): what its mirror says changed since the last frame —
/// the node's LOCAL transform (the engine composes its own hierarchy, so a moved subtree root is ONE record, never its
/// subtree), its parent, its graph class. Only the named `fields` are meant. Applied at the top of the frame, in order,
/// before the change set (OgreScene::applyNodeRecords), on a node made by Scene::createDocumentNode.
struct NodeRecord {
    enum : unsigned char { Trs = 1, Parent = 2, Static = 4 };
    NodeId        id = 0;
    unsigned char fields = 0;
    bool          isStatic = false;   ///< Static: the GRAPH CLASS (SCENE_STATIC), the document's decision
    NodeId        parent = 0;         ///< Parent: 0 = the scene root
    Vec3          position;           ///< Trs: the LOCAL transform
    Quat          rotation;
    Vec3          scale = Vec3(1.0f, 1.0f, 1.0f);
};
struct ChangeSet {
    bool                complete = true;
    std::vector<NodeId> moved;
    bool empty() const { return complete && moved.empty(); }
};
/// A STRUCTURAL MESSAGE (ENGINE-UPGRADE-U3 stage 3): one command of the host's sync — a create (under the id the host
/// reserved), a destroy, an attach, a material, a light, a decal, a socket, a texture — carried by the record and
/// applied, in the order the host emitted it, at the top of the frame that takes the record, BEFORE the node records
/// (which may name what it creates). It owns what it carries (the pixels, the mesh data): nothing points back into
/// the host. A command the engine refuses says so in the frame's errors (PublishedEngineFrame::frameErrors).
struct SceneMessage {
    std::function<bool(Scene &)> apply;   ///< the command; its answer (false = refused) is the completion's
    /// A host that must learn the answer names the message (non-zero): its completion — ok or refused, with the
    /// engine's reason — is published in Scene::published().completions (U3 stage 3).
    unsigned long long token = 0;
    /// THE ONE ORDER (decision 46): stamped when the message reaches the slot — the drain applies every stream's
    /// messages (the engine's, every scene's, every view's binding) in this order.
    unsigned long long seq = 0;
};

/// The user's GESTURE as the host knows it (a gizmo drag, a viewport drag, a material hover): what makes a moving
/// still object a DRAG MOVER for its length (GiParams::dragMoverChannel) and ends it — the engine infers nothing from
/// motion (U0 3c). Per scene (a NodeId is a scene's own), per frame: a drain without one reads "no gesture".
struct FrameInputGesture {
    bool                active = false;
    std::vector<NodeId> nodes;   ///< the nodes the gesture moves (each implies its subtree)
    bool operator==(const FrameInputGesture &o) const { return active == o.active && nodes == o.nodes; }
};
struct FrameInputScene {
    /// THE MOST STEPS ONE FRAME HANDS A SCENE (ONE-FRAME-1; the stage-1 audit's bug 1): the document's own catch-up bound
    /// (iris::SimulationClock::kMaxStepsPerAdvance, asserted equal in the mirror). Steps owed past it when the scene is
    /// next drawn are DROPPED: time nobody saw is not shown at once.
    static constexpr unsigned long long kMaxStepsPerFrame = 8;
    Scene *scene = nullptr;
    /// THE GI SCHEDULER'S INPUTS that only the document knows (U0 3c; read only on a record that carries `gi`):
    /// world.refreshGi()'s serial (a change = an explicit re-solve) and a material HOVER PREVIEW on screen (the
    /// material term holds still: a preview is no edit). Merged newest / by OR.
    std::optional<unsigned long long> giRefreshSerial;
    bool                              materialPreview = false;
    std::optional<FrameInputGesture>  gesture;
    /// Never compared at the drain like the fields below: a set is EVENTS, applied once, merged by appending.
    ChangeSet changes;
    /// THE DOCUMENT NODES' RECORDS (the extract, NodeRecord): events too — applied in order, merged by appending.
    std::vector<NodeRecord> nodes;
    /// THE STRUCTURAL MESSAGES, in emission order (U3 stage 3) — applied before `nodes`.
    std::vector<SceneMessage> messages;
    std::optional<ShadowDesc>             shadows;
    std::optional<Colour>                 environmentLight;
    std::optional<EnvironmentDesc>        environment;   ///< R5b: what the environment capture photographs
    std::optional<FogDesc>                fog;
    std::optional<HeightFogDesc>          heightFog;
    std::optional<RayTracingMode>         rayTracing;
    std::optional<SunContactDesc>         sunContact;
    std::optional<GiParams>               gi;          ///< a tuning-only change applies through setGiTuning
    std::optional<PlanarReflectionParams> planar;
    std::optional<SkyDesc>                sky;
    std::optional<float>                  shaderTime;
    std::optional<GridDesc>               grid;
    // ---- THE SCENE'S CLOCK (ONE-FRAME-1 stage 1, V2 plan 2.1). ONE WRITER: the scene's mirror (SceneMirror's emit), from
    // its document's iris::SimulationClock — every host (the editor, the player, a preview, a thumbnail) steps a scene
    // only by advancing its document. The engine keeps the cursor WITH THE SCENE: a frame advances the scene by
    // (serial - the last serial it saw) x stepPeriod — ONCE (a second sync of the same steps moves nothing) and 0 on a
    // frame nobody stepped it (an empty hand-over, a picture's settle). Documents are plural, so time is per scene:
    // two hosts in one hand-over are two scenes' parts and never collide. `reassert` re-primes the cursor (a new
    // document on the same scene). Everything the engine simulates for the scene reads the result: its particles
    // (x particleTimeScale, the fork's ParticleSystemManager2::setTimeSinceLast), the HDR meter and the looks of every
    // view showing it, its cloud clock, its interpolation (the newest grid state). Newest wins in a merge (a serial
    // is monotone, so the newest is the right one).
    /// The document clock's LIFETIME step count (iris::SimulationClock::lifetimeSteps: never reset by a play edge) the
    /// part was built at — also the grid state its node records are (the interpolation draws between the last two).
    std::optional<unsigned long long>     stepSerial;
    /// THE GRID'S STEP, in seconds (iris::SimulationClock::kStepSeconds, 1/60); 1/60 until a part says.
    std::optional<float>                  stepPeriod;
    /// The document's particle time scale: the scene's particles advance by its steps x stepPeriod x this, and
    /// nothing else of the scene reads it (a slow-motion flame never slows the exposure). Persists; 1 until a part says.
    std::optional<float>                  particleTimeScale;
    /// THE DOCUMENT the serial counts (ONE-FRAME-1, the stage-1 audit's bug 2): the scene's mirror moves it at every
    /// setSource, and only a moved epoch re-primes the scene's cursor (a new document's serial is another clock's). A
    /// `reassert` (re-apply every field: a page switch, a re-take) keeps the steps. Newest wins in a merge.
    std::optional<unsigned long long>     clockEpoch;
    /// THE VIEW'S PIXEL-ERROR THRESHOLD (ATOM-3 stage 1): the World Mode's `atomPixelError` row, in pixels
    /// (Scene::setPixelTolerance).
    std::optional<float>                  pixelTolerance;
    /// RE-ASSERT: forget what this scene AND EVERY VIEW SHOWING IT last applied from a record (GI excepted — a GI
    /// push is a from-scratch rebuild, ENGINE_CACHE_POLICY_SPEC P10) so every field present applies again. A host
    /// retaking the screen asks for it: part of what the scene's fields set is process-wide backend state another
    /// scene may have changed meanwhile, and a view's chain may have been written outside the record. One flag on the
    /// scene, so a record that carries no view part this frame cannot use it up before the views' parts arrive.
    bool                                  reassert = false;
};
struct FrameInputView {
    View *view = nullptr;
    std::optional<CameraDesc> camera;
    std::optional<NodeId>     cameraNode;   ///< 0 = the camera rides no node
    std::optional<PostFxDesc> postFx;
    std::optional<bool>       shadows;
    std::optional<unsigned>   sampleCount;
    std::optional<Colour>     background;
    // ---- THE HOST'S VIEW SETTERS AS FIELDS (ENGINE-UPGRADE-U3 stage 4a, decision 39 a): each View setter called on
    // the host's side writes its field here (the newest wins in a merge) and the drain applies it — never inline.
    std::optional<Scene *>            scene;              ///< View::setScene (null = detach); the drain rebinds
    unsigned long long                sceneSeq = 0;       ///< the binding's place in the one order (decision 46)
    std::optional<bool>               enabled;            ///< View::setEnabled
    std::optional<OffscreenContract>  offscreenContract;  ///< View::setOffscreenContract
    std::optional<bool>               helpersVisible;     ///< View::setHelpersVisible
    std::optional<bool>               vrHelpersVisible;   ///< View::setVrHelpersVisible
    std::optional<bool>               asyncShaders;       ///< View::setAsyncShaders
    std::optional<ViewOverlayDesc>    overlay;            ///< View::setOverlay
    std::optional<ViewPipDesc>        pip;                ///< View::setPip
    std::optional<bool>               lodHysteresisOffscreen;   ///< View::setLodHysteresisOffscreen
    std::optional<std::pair<unsigned, unsigned>> size;    ///< View::resize (the target's new size; before the camera)
    bool                      resetExposure = false;   ///< a camera cut: the exposure history re-seeds once
    /// Seed the exposure history with this scale (View::seedExposureHistory) — after the chain lands, since the
    /// seed is derived from it (a new recorder view starts at the editor's measured exposure).
    std::optional<float>      seedExposure;
};
/// AN ENGINE MESSAGE (ENGINE-UPGRADE-U3 stage 4a, decision 39 c): an engine verb the host called — the VR session's,
/// the shader cache's, the monitor's, a create — carried in the record and applied at the top of the drain, BEFORE
/// the scenes' streams (a scene a message creates exists for its own commands), in the order the host called them.
/// Its answer is its completion (PublishedEngineFrame::completions), the scene messages' shape.
struct EngineMessage {
    std::function<bool(Engine &)> apply;
    unsigned long long token = 0;
    unsigned long long seq = 0;   ///< the one order across streams (decision 46), stamped at the slot
};
struct FrameInput {
    std::vector<EngineMessage>        engineMessages;  ///< the engine's own stream (decision 39 c), drained first
    std::optional<FrameCause>         cause;           ///< why the next frame renders (resets to Driver)
    std::optional<unsigned>           shadowMapBudget; ///< engine-wide (the shadow map pool's size)
    bool                              refreshShadows = false;   ///< the document asked for a shadow re-render
    std::vector<FrameInputScene>      scenes;
    std::vector<FrameInputView>       views;
    // ---- THE HAND-OVER (ENGINE-UPGRADE-U3 stage 1, the seam) ----
    /// THE RECORD'S SERIAL: stamped by the ENGINE when the render loop takes the record accumulated since the last
    /// hand-over at its frame head — one serial per frame that applies a record (an empty hand-over keeps the last).
    /// A submitter never sets it: submitFrame ignores the field. PublishedEngineFrame::serial is the one a frame applied.
    unsigned long long                serial = 0;
    /// The usual one-field record.
    static FrameInput withCause(FrameCause c) { FrameInput f; f.cause = c; return f; }
};
/// WHAT THE SUBMITS DID — Engine::frameSubmitStatus(): the GAME side of the hand-over (ENGINE-UPGRADE-U3 F4: each half
/// of the record's status is written by one thread; this half by the thread that submits). Running totals.
struct FrameSubmitStatus {
    unsigned long long submitted = 0;     ///< submitFrame calls
    unsigned long long messagesPosted = 0;   ///< Engine::post calls (one per structural message, decision 35)
    /// Engine::request calls the HOST made (off the engine's side; FRAME-MONITOR-DOOR-1): a request waits up to a frame
    /// in free-running, so the per-tick path makes none — perf.capture_free_tick holds a capture's ticks to 0 here.
    unsigned long long requests = 0;
    /// Scene parts MERGED into an earlier pending part of the same scene (field by field, the newest value wins) —
    /// every second submit for a scene before one hand-over counts, the host's several syncs a frame included.
    unsigned long long scenePartsMerged = 0;
    /// THE SERIAL THE PENDING RECORD WILL BE APPLIED AS (the next hand-over's; the last one's when nothing is pending):
    /// what the LOCKSTEP verb waits for — "a frame with serial >= this presented" (F2).
    unsigned long long nextSerial = 0;
    /// THE HAND-OVERS SO FAR (U3 stage 5): every frame's head takes the slot once — a record submitted after the
    /// n-th is applied by the frame of the (n+1)-th (frame.drag_latency's law: never two frames late).
    unsigned long long handOvers = 0;
};
/// What the drain did — Engine::frameInputStatus() / PublishedEngineFrame::input: the RENDER side of the hand-over,
/// written by the frame alone (U3 F4). Running totals except the last drain's fields.
struct FrameInputStatus {
    unsigned long long drained = 0;       ///< frames that drained (one per renderOneFrame)
    unsigned long long drainedEmpty = 0;  ///< ...that found no record submitted since the last (an EMPTY hand-over)
    unsigned           fieldsApplied = 0; ///< fields the LAST drain applied (changed values only)
    unsigned           viewFieldsApplied = 0;   ///< ...of them, the per-view ones
    bool               gestureActive = false;   ///< the last drain carried an active gesture for some scene
    // ---- the change set (U0 3b) ----
    unsigned           changesMoved = 0;        ///< `moved` entries the LAST drain applied, over every scene
    unsigned           changesLights = 0;       ///< ...of whose subtrees held a light (each scene bumps its serial once)
    unsigned long long changesIncomplete = 0;   ///< incomplete sets drained, ever (each one cost a full pass)
    // ---- the extract (ENGINE-UPGRADE-U2) ----
    unsigned           nodeRecords = 0;         ///< NodeRecords the LAST drain applied, over every scene
    unsigned           nodeTransforms = 0;      ///< ...of them, the ones carrying a transform (the written nodes)
    // ---- the structural messages (ENGINE-UPGRADE-U3 stage 3) ----
    unsigned           messages = 0;            ///< structural messages the LAST drain applied, over every scene
    unsigned long long messagesTotal = 0;       ///< ...ever
    unsigned long long messagesRefused = 0;     ///< ...ever refused (each one a completion of its own, decision 34)
    unsigned long long nodeRecordsTotal = 0;    ///< ...ever
    /// THE FREE FRAME'S BUDGET (BUGS-7): the messages the LAST drain left to the next frame (its budget
    /// spent), and the drains that were cut, ever. Lockstep frames, joins and requests drain whole (0 here).
    unsigned           messagesCarried = 0;
    unsigned long long drainsCut = 0;
    double             drainMicros = 0.0;       ///< what the LAST drain cost (the record's own overhead)
    /// ...THE LAST DRAIN'S PARTS (ENGINE-UPGRADE-U4 stages 1, 3; the monitor's stage `engine.drain` and its children
    /// time the same spans): NAMING the moved subtrees to the scans (applyChangeSet -> nameMovedSubtrees), the NODE
    /// RECORDS (applyNodeRecords alone) and, in free mode, the INTERPOLATION (interpolateScene + the reflector arm),
    /// summed over every scene; the rest of drainMicros is the messages, the fields and the views. (The GI
    /// scheduler's plan left the drain in stage 3: `engine.plan`.)
    double             drainNameMicros = 0.0;
    double             drainRecordsMicros = 0.0;
    double             drainInterpolateMicros = 0.0;
    /// ...AND ITS COUNTS (ENGINE-UPGRADE-U4 decision 15 — what the drain row asserts at every arm; the µs bars are per
    /// arm): the index lookups the node records made (one per record, one more for a record that names a parent),
    /// and the naming passes' node visits and the index lookups they made (each node of a moved subtree once, one
    /// lookup per visit). A scan creeping back shows here as a count, whatever the box's load.
    unsigned           drainRecordLookups = 0;
    unsigned           drainNameVisits = 0;
    unsigned           drainNameLookups = 0;
};

/// THE ENGINE'S NAME FOR AN ATTACHED CLIP (ENGINE-UPGRADE-U3 stage 3) — the ONE rule, used by the engine when it
/// attaches (Scene::attachClips) and by a host that must know the name the moment it pushes (the record lands a frame
/// later): the clip's own name, or "clip<index>" (its index on the node) when it has none, uniquified against the
/// names the node already carries with " 2", " 3", … (Ogre's skeleton finds a clip by name, the first match).
template <class Taken>
std::string clipNameFor(const std::string &descName, size_t index, Taken &&taken)
{
    std::string name = descName.empty() ? ("clip" + std::to_string(index)) : descName;
    const std::string base = name;
    int suffix = 2;
    while (taken(name)) name = base + " " + std::to_string(suffix++);
    return name;
}

}}  // namespace jahshaka::engine
