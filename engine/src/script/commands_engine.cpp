// THE SHAPE OF EVERY COMMAND (the lead's rule, 2026-11-22; Registry::validate enforces it, jahshaka/engine/Script.h):
// THE
// TARGETS ARE POSITIONAL — the scene; then the node, view or material it acts on; the picture a reduction reads — and
// EVERYTHING ELSE IS NAMED, in at most one further parameter: one scalar (engine.frames(n), engine.scene.create(name)) or
// ONE options object. A misspelt name FAILS in the strict host; two swapped positional values could not. A command that
// answers a handle names its kind in its doc ("-> scene handle"). Vectors and colours are plain arrays of numbers, their
// length checked by the host.
//
// THE ENGINE'S COMMANDS (SCRIPT-IN-ENGINE-1): the engine registers its `engine.*` commands into the process's script
// host (jahshaka/engine/Script.h, registerEngineCommands), ONE FILE PER FAMILY - commands_scene.cpp (scene, mesh,
// material), commands_node.cpp, commands_view.cpp, commands_gi.cpp, commands_picture.cpp, and this file (the frames and
// the arms; the registration of every family) - over the state and helpers of commands.h. A new family is a new file and
// one line in registerEngineCommands below. Every command calls the PUBLIC engine
// API on the calling thread, which is the engine's (the host calls commands inline; Engine.h's thread affinity).
// EVERY OBJECT IS A TYPED HANDLE (Script.h, THE HANDLES): scenes, views and pictures held by the host; nodes, meshes
// and materials naming the engine's own ids, their existence asked of the engine at every call. A picture is the request shape of PICTURE-REQUEST-1 (view, eyes, content, when); its reductions are
// NATIVE commands (the script language is an interpreter: a pixel loop is never a script's).
//
// THE FRAME's PUBLIC DOOR (2b b3; ruling 6): a frame steps every scene THIS HOST MADE by one grid step through the
// product's own door - its FrameInputScene part's stepSerial (+1 a frame) handed over by Engine::submitFrame before
// renderOneFrame: a script's scenes have no document, so the host that made them is their clock's ONE writer
// (ONE-FRAME-1). A scene the host's owner handed in keeps its owner's clock: this host never stamps it.
//
// ONE READ GOES THROUGH THE FIXTURES' DOOR (EngineInspect.h), and only this, because the C++ fixtures' harness has
// no public equivalent and a script row must give the same route judgement as the fixture it replaces: THE ENGINE's
// OWN CHAIN RECORD (inspect::ViewEcho::routes) that engine.view.declareRoutes' judgement holds the published row against
// (the two doors agree: requireRoutes' law). The pause is the published record's (GiStatus::updateBudget == 0), for the
// fixtures as for the scripts. R11 (source.testing_rules) bars that header from
// Studio's src/ and irisgl/mirror, the hosts; this is the engine's own file.
//
// THE OWNERSHIP LAW (2b; the lead, for the lab's fresh host per Run): an engine object A COMMAND MADE is the host's -
// a scene or a view held OWNED (its release destroys it: engine.scene.destroy / engine.view.destroy, or the host's
// end), a node, mesh or material RECORDED by its scene and removed at the host's end (a scene the host's owner handed
// in outlives the host; what a run made in it must not) - nodes first, then the meshes and materials they drew (trap
// 1's order), then the views, then the scenes (the registry's reverse release). An object the host's OWNER handed in
// (held unowned, by its owner's call) is never destroyed - and IN A HANDED-IN SCENE A SCRIPT CHANGES OR DESTROYS ONLY
// WHAT ITS OWN HOST MADE (Commands::mine: the made record is the authority; the owner's nodes, meshes and materials are
// read-only to it, and the scene's own settings too: Commands::settable); a handed-in VIEW is the script's to drive,
// never to destroy. Every command that changes a node, mesh, material or a scene's settings asks them. THE HOST DIES BEFORE ITS ENGINE (registerEngineCommands'
// contract): a host that ends after its engine is a named fault (stderr; it aborts a debug build) and calls nothing.
#include "script/commands.h"

#include "jahshaka/engine/EngineInspect.h"
#include "jahshaka/engine/GiStatus.h"
#include "jahshaka/engine/Monitor.h"
#include "jahshaka/engine/Published.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "jahshaka/engine/FrameInput.h"
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Pictures.h"

namespace jahshaka { namespace engine { namespace script {

namespace {
/// False, the end's fault said once, when the host's engine is gone (the deleters then call nothing).
bool engineLive(EngineState &st)
{
    if (!st.engine || st.engineGone) return false;
    if (Engine::isAlive()) return true;
    st.engineGone = true;
    std::fprintf(stderr, "FAULT: a script host ended after its engine (registerEngineCommands: the host dies first) - "
                         "nothing it made is destroyed by it\n");
#ifndef NDEBUG
    std::abort();
#endif
    return false;
}
}  // namespace

std::shared_ptr<Scene> ownedScene(const std::shared_ptr<EngineState> &st, Scene *s)
{
    st->ownedScenes.push_back(EngineState::OwnedScene{s, 0});
    return std::shared_ptr<Scene>(s, [st](Scene *dead) {
        forgetScene(*st, dead);
        st->ownedScenes.erase(std::remove_if(st->ownedScenes.begin(), st->ownedScenes.end(),
                                             [dead](const EngineState::OwnedScene &o) { return o.scene == dead; }),
                              st->ownedScenes.end());
        st->declared.erase(std::remove_if(st->declared.begin(), st->declared.end(),
                                          [dead](const DeclaredRoutes &d) { return d.scene == dead; }),
                           st->declared.end());
        if (engineLive(*st)) st->engine->destroyScene(dead);
    });
}

std::shared_ptr<View> ownedView(const std::shared_ptr<EngineState> &st, View *v)
{
    st->ownedViews.push_back(v);
    return std::shared_ptr<View>(v, [st](View *dead) {
        st->ownedViews.erase(std::remove(st->ownedViews.begin(), st->ownedViews.end(), dead), st->ownedViews.end());
        st->declared.erase(std::remove_if(st->declared.begin(), st->declared.end(),
                                          [dead](const DeclaredRoutes &d) { return d.view == dead; }),
                           st->declared.end());
        if (engineLive(*st)) st->engine->destroyView(dead);
    });
}

void noteMade(EngineState &st, Scene *scene, MadeKind kind, std::uint32_t id) { st.made[scene].insert(madeKey(kind, id)); }

void forgetMade(EngineState &st, Scene *scene, MadeKind kind, std::uint32_t id)
{
    const auto it = st.made.find(scene);
    if (it == st.made.end()) return;
    it->second.erase(madeKey(kind, id));
    if (it->second.empty()) st.made.erase(it);
}

void forgetScene(EngineState &st, Scene *scene) { st.made.erase(scene); }

namespace {
bool ownedBy(const EngineState &st, const Scene *s)
{
    return std::any_of(st.ownedScenes.begin(), st.ownedScenes.end(),
                       [s](const EngineState::OwnedScene &o) { return o.scene == s; });
}
}  // namespace

bool Commands::mine(Call &c, Scene *s, MadeKind kind, std::uint32_t id, const char *verb) const
{
    if (ownedBy(*st, s)) return true;
    const auto it = st->made.find(s);
    if (it != st->made.end() && it->second.count(madeKey(kind, id))) return true;
    static const char *const kWord[] = {"node", "mesh", "material"};
    c.fail(std::string(kWord[int(kind)]) + " " + std::to_string(id) + " is not this host's to " + verb + ": the scene '" +
           s->name() + "' was handed in by the host's owner, and this host did not make it");
    return false;
}

bool Commands::settable(Call &c, const Scene *s) const
{
    if (ownedBy(*st, s)) return true;
    c.fail("the scene '" + s->name() + "' was handed in by the host's owner: its settings are not this host's to change");
    return false;
}

bool Commands::frame(Call &c) const
{
    if (!st->ownedScenes.empty()) {
        FrameInput step = FrameInput::withCause(FrameCause::Scripted);
        for (EngineState::OwnedScene &o : st->ownedScenes) {
            FrameInputScene part;
            part.scene = o.scene;
            part.stepSerial = ++o.stepSerial;
            step.scenes.push_back(std::move(part));
        }
        st->engine->submitFrame(std::move(step));
    }
    st->engine->renderOneFrame();
    std::string broken;
    for (size_t i = 0; i < st->declared.size();) {
        DeclaredRoutes &d = st->declared[i];
        RouteWords ran, pub;
        if (const inspect::ViewEcho *ve = inspect::view(d.view)) {
            const inspect::ViewRoutes vr = ve->routes();
            ran.gather = vr.probeGather; ran.integrate = vr.screenIntegrate; ran.reflection = vr.rayReflect;
        }
        const GiStatus g = d.scene->published().gi;
        ran.field = g.ifdBound;
        ran.cards = g.cards.built;
        ran.store = !g.cascades.empty();
        ran.paused = g.updateBudget == 0;
        const ViewFrames row = d.view->published();
        pub.gather = row.routeGather; pub.integrate = row.routeIntegrate; pub.reflection = row.routeReflect;
        std::string line;
        const RouteJudgement j = judgeRoutes("view '" + d.view->name() + "'", d.words, ran, row.id != 0, pub,
                                             ++d.frames, line);
        if (j == RouteJudgement::Wait) {
            ++i;
            continue;
        }
        st->host->law(j == RouteJudgement::Held, line);
        if (j == RouteJudgement::Broken && broken.empty()) broken = line;
        st->declared.erase(st->declared.begin() + long(i));
    }
    if (broken.empty()) return true;
    c.fail(broken);
    return false;
}

void registerFrameCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    r.add({"engine.arm", {Access::Changes,
                          {object("arm", {arg("name", Kind::String), arg("value", Kind::Number)})}, Kind::Bool},
           "Sets the engine's measuring arm {name, value} (Engine::arms). A TEST arm opens only in a host started with "
           "--test-doors. An arm is engine-wide: a host for an outside client refuses it."},
          [k](Call &c) -> Value {
              if (k.st->host->options().outsideClient)
                  return c.fail("arms are engine-wide: this host serves an outside client");
              Engine *e = k.engine(c);
              if (!e) return Value();
              const std::string name = c[0]["name"].asString();
              bool known = false, test = false;
              for (const ArmInfo &a : e->arms())
                  if (a.name == name) { known = true; test = a.test; }
              if (!known) return c.fail("no arm is named '" + name + "'");
              if (test && !k.st->testDoors) return c.fail("'" + name + "' is a test arm: the host was not started with --test-doors");
              bool ok = false;
              const double value = c[0]["value"].asNumber();
              e->request([&](Engine &x) { ok = x.setArm(name, value); });
              if (!ok) return c.refuse(e->lastError());
              return Value(true);
          });
    r.add({"engine.frames", {Access::Changes, {arg("n", Kind::Int).atLeast(0).in("frames")}, Kind::Null},
           "Draws n frames in lockstep, each one grid step (1/60 s) of every scene this host made; returns when the "
           "n-th frame is drawn and published."},
          [k](Call &c) -> Value {
              if (!k.engine(c)) return Value();
              const std::int64_t n = c[0].asInt();
              for (std::int64_t i = 0; i < n; ++i) {
                  // THE KEEP-GOING POLL at each frame boundary (Host::keepGoing: a caller's cancel, and its progress)
                  if (!k.st->host->keepGoing(double(i), double(n)))
                      return c.fail("cancelled at frame " + std::to_string(i) + " of " + std::to_string(n));
                  if (!k.frame(c)) return Value();
              }
              k.st->host->keepGoing(double(n), double(n));   // the end, as progress: every frame is drawn
              return Value();
          });
}

void registerEngineCommands(Host &host, Engine *engine, std::string bootError, bool testDoors)
{
    auto st = std::make_shared<EngineState>();
    st->host = &host;
    st->engine = engine;
    st->bootError = std::move(bootError);
    st->testDoors = testDoors;
    st->pictureBytesCap = host.options().pictureBytesCap;
    // THE HOST's END (THE OWNERSHIP LAW above): what the commands made in a scene they do not own, removed - every node
    // first (a node draws a mesh and a material), then the meshes, then the materials; the views and scenes they made
    // follow, released by the registry (the last held first)
    host.registry().atEnd([st] {
        if (!engineLive(*st)) return;
        for (const MadeKind kind : {MadeKind::Node, MadeKind::Mesh, MadeKind::Material})
            for (const auto &[scene, keys] : st->made)
                for (auto it = keys.rbegin(); it != keys.rend(); ++it) {   // (the highest id first)
                    if (MadeKind(*it >> 32) != kind) continue;
                    const std::uint32_t id = std::uint32_t(*it);
                    if (kind == MadeKind::Node) scene->removeNode(NodeId(id));
                    else if (kind == MadeKind::Mesh) scene->destroyMesh(MeshId(id));
                    else scene->destroyMaterial(MaterialId(id));
                }
        st->made.clear();
    });
    const Commands k(st);
    // THE KINDS BOUND AT REGISTRATION (one kind, one type - never "whoever holds first"): what each held kind's handle
    // names, and the kinds that carry the engine's own ids
    host.registry().bindKind<Scene>(kScene);
    host.registry().bindKind<View>(kView);
    host.registry().bindKind<Image>(kPicture);
    host.registry().nameKind(kNode);
    host.registry().nameKind(kMesh);
    host.registry().nameKind(kMaterial);
    registerSceneCommands(host, k);
    registerViewCommands(host, k);
    registerNodeCommands(host, k);
    registerGiCommands(host, k);
    registerFrameCommands(host, k);
    registerPictureCommands(host, k);
    registerMaterialCommands(host, k);
    registerLightCommands(host, k);
    registerEffectsCommands(host, k);
    registerAtomCommands(host, k);
    registerStatusCommands(host, k);
    // THE EXISTENCE OF THE ENGINE's OWN IDS (THE HANDLES, Script.h; the lead's S1): a node, mesh or material handle names
    // the engine's id, and the ENGINE is asked (Scene::nodePresence...) whether its scene - the command's first target -
    // holds it now: a dead or foreign id FAILS by name before the command runs (Scene::setNodeTransform would ignore
    // it). In the render thread's mode the read is unanswered and the call FAILS saying so (the editor client's door, P3).
    const auto presence = [](const char *kind, Scene::Presence (*ask)(const Scene &, std::uint32_t)) {
        return [kind, ask](const Call &c, std::uint32_t id, std::string &why) {
            const Scene *s = c.info().signature.params.empty() || c.info().signature.params[0].handle != kScene
                                 ? nullptr : c.object<Scene>(c[0]);
            if (!s) {
                why = std::string("a ") + kind + " is named with its scene: this command's first target names no engine "
                      "scene";
                return false;
            }
            const Scene::Presence p = ask(*s, id);
            if (p == Scene::Presence::Present) return true;
            why = p == Scene::Presence::Absent
                      ? std::string(kind) + " " + std::to_string(id) + " is not a " + kind + " of scene '" + s->name() +
                            "' (another scene's, or destroyed)"
                      : std::string("the ") + kind + " cannot be checked: a render thread owns the engine (its "
                        "existence read is unanswered there)";
            return false;
        };
    };
    host.registry().checkKind(kNode, presence(kNode, [](const Scene &s, std::uint32_t id) { return s.nodePresence(id); }));
    host.registry().checkKind(kMesh, presence(kMesh, [](const Scene &s, std::uint32_t id) { return s.meshPresence(id); }));
    host.registry().checkKind(kMaterial, presence(kMaterial, [](const Scene &s, std::uint32_t id) {
                                  return s.materialPresence(id);
                              }));
}

}}}  // namespace jahshaka::engine::script
