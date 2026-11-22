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
// SCENES AND VIEWS ARE INTEGER HANDLES (1, 2, ... in creation order, never reused); nodes, meshes and materials are the
// engine's own ids. A picture is the request shape of PICTURE-REQUEST-1 (view, eyes, content, when); its reductions are
// NATIVE commands (the script language is an interpreter: a pixel loop is never a script's).
//
// THREE READS GO THROUGH THE FIXTURES' DOOR (EngineInspect.h), and only these, because the C++ fixtures' harness has
// no public equivalent and a script row must give the same picture and the same route judgement as the fixture it
// replaces: (1) THE HARNESS's GRID — engine.frames steps every scene no host ever stamped by one grid step before each
// frame (inspect::EngineEcho::harnessStep, enginetest::frames' law "a frame is one grid step"): an engine-only script's
// scenes have no document, so nothing else gives them time; (2) THE PAUSE in engine.view.routes and the route
// judgement (inspect::SceneEcho::giUpdateBudget() == 0), the one route word the published record does not carry;
// (3) THE ENGINE's OWN CHAIN RECORD (inspect::ViewEcho::routes) that engine.view.declareRoutes' judgement holds the
// published row against (the two doors agree: requireRoutes' law). R11 (source.testing_rules) bars that header from
// Studio's src/ and irisgl/mirror, the hosts; this is the engine's own file.
#include "script/commands.h"

#include "jahshaka/engine/EngineInspect.h"

namespace jahshaka { namespace engine { namespace script {

bool Commands::frame(Call &c) const
{
    if (const inspect::EngineEcho *echo = inspect::engine(st->engine))
        const_cast<inspect::EngineEcho *>(echo)->harnessStep(1);
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
        const inspect::SceneEcho *se = inspect::scene(d.scene);
        ran.paused = se && se->giUpdateBudget() == 0;
        const PublishedEngineFrame::ViewFrames row = d.view->published();
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
    r.add({"engine.arm", {{object("arm", {arg("name", Kind::String), arg("value", Kind::Number)})}, Kind::Bool},
           "Sets the engine's measuring arm {name, value} (Engine::arms). A TEST arm opens only in a host started with "
           "--test-doors."},
          [k](Call &c) -> Value {
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
    r.add({"engine.frames", {{arg("n", Kind::Int)}, Kind::Null},
           "Draws n frames in lockstep, each one grid step (1/60 s) of every scene no host stamps; returns when the "
           "n-th frame is drawn and published."},
          [k](Call &c) -> Value {
              if (!k.engine(c)) return Value();
              const std::int64_t n = c[0].asInt();
              if (n < 0) return c.fail("n must be 0 or more");
              for (std::int64_t i = 0; i < n; ++i)
                  if (!k.frame(c)) return Value();
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
    const Commands k(st);
    registerSceneCommands(host, k);
    registerViewCommands(host, k);
    registerNodeCommands(host, k);
    registerGiCommands(host, k);
    registerFrameCommands(host, k);
    registerPictureCommands(host, k);
}

}}}  // namespace jahshaka::engine::script
