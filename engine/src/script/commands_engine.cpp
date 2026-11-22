// THE SHAPE OF EVERY COMMAND (the lead's rule, 2026-11-22; Registry::validate enforces it, jahshaka/engine/Script.h): THE
// TARGETS ARE POSITIONAL — the scene; then the node, view or material it acts on; the picture a reduction reads — and
// EVERYTHING ELSE IS NAMED, in at most one further parameter: one scalar (engine.frames(n), engine.scene.create(name)) or
// ONE options object. A misspelt name FAILS in the strict host; two swapped positional values could not. A command that
// answers a handle names its kind in its doc ("-> scene handle"). Vectors and colours are plain arrays of numbers, their
// length checked by the host.
//
// THE ENGINE'S COMMANDS (SCRIPT-IN-ENGINE-1 stage 1): the engine registers its `engine.*` commands into the process's
// script host (jahshaka/engine/Script.h, registerEngineCommands). Stage 1 carries the ones one fixture needs
// (rtreflect.odd_size, tests/rtreflect/odd_size.js); stage 2 the rest by family. Every command calls the PUBLIC engine
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
#include "jahshaka/engine/Engine.h"
#include "jahshaka/engine/EngineInspect.h"
#include "jahshaka/engine/Script.h"
#include "script/RouteDeclaration.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace jahshaka { namespace engine { namespace script {

namespace {

/// A route declaration not yet judged held (engine.view.declareRoutes): its view, its scene, the words, the frames since.
struct DeclaredRoutes {
    View *view = nullptr;
    Scene *scene = nullptr;
    RouteWords words;
    int frames = 0;
};

struct EngineState {
    Host *host = nullptr;
    Engine *engine = nullptr;
    std::string bootError;
    bool testDoors = false;
    std::vector<Scene *> scenes;   // handle - 1
    std::vector<View *> views;     // handle - 1
    std::vector<DeclaredRoutes> declared;
};

Value vec(const Vec3 &v) { return Value(Value::Array{Value(double(v.x)), Value(double(v.y)), Value(double(v.z))}); }
Value rgb(const Colour &c) { return Value(Value::Array{Value(double(c.r)), Value(double(c.g)), Value(double(c.b))}); }
Vec3 toVec3(const Value &v)
{
    const Value::Array &a = v.asArray();
    return Vec3(float(a[0].asNumber()), float(a[1].asNumber()), float(a[2].asNumber()));
}
Colour toColour(const Value &v)
{
    const Value::Array &a = v.asArray();
    return Colour(float(a[0].asNumber()), float(a[1].asNumber()), float(a[2].asNumber()));
}

/// The picture a readPixels answered: {width, height, rgba}.
Param picture(const char *name)
{
    return object(name, {arg("width", Kind::Int), arg("height", Kind::Int), arg("rgba", Kind::Bytes)});
}

/// The camera pose at `pos` looking at `target`, +Y up (testCameraDescLookAt's arithmetic: the same quaternion).
Quat lookAtRotation(const Vec3 &pos, const Vec3 &target)
{
    const Vec3 f0{target.x - pos.x, target.y - pos.y, target.z - pos.z};
    const float fl = std::sqrt(f0.x * f0.x + f0.y * f0.y + f0.z * f0.z);
    const Vec3 f{f0.x / fl, f0.y / fl, f0.z / fl};
    const Vec3 upW{0.f, 1.f, 0.f};
    Vec3 r{f.y * upW.z - f.z * upW.y, f.z * upW.x - f.x * upW.z, f.x * upW.y - f.y * upW.x};
    const float rl = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    r = Vec3{r.x / rl, r.y / rl, r.z / rl};
    const Vec3 u{r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
    const float m00 = r.x, m01 = u.x, m02 = -f.x;
    const float m10 = r.y, m11 = u.y, m12 = -f.y;
    const float m20 = r.z, m21 = u.z, m22 = -f.z;
    const float tr = m00 + m11 + m22;
    if (tr > 0.f) {
        const float sq = std::sqrt(tr + 1.f) * 2.f;
        return Quat{(m21 - m12) / sq, (m02 - m20) / sq, (m10 - m01) / sq, 0.25f * sq};
    }
    if (m00 > m11 && m00 > m22) {
        const float sq = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        return Quat{0.25f * sq, (m01 + m10) / sq, (m02 + m20) / sq, (m21 - m12) / sq};
    }
    if (m11 > m22) {
        const float sq = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        return Quat{(m01 + m10) / sq, 0.25f * sq, (m12 + m21) / sq, (m02 - m20) / sq};
    }
    const float sq = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
    return Quat{(m02 + m20) / sq, (m12 + m21) / sq, 0.25f * sq, (m10 - m01) / sq};
}

/// A unit cube, 24 vertices, positions + per-face normals, no uvs (enginetest::unitCubeMesh's geometry).
MeshData unitCube()
{
    MeshData d;
    const float h = 0.5f;
    const float fn[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    const float fv[6][4][3] = {{{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}},
                               {{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}},
                               {{h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}},
                               {{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}},
                               {{-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}},
                               {{-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}}};
    for (int f = 0; f < 6; ++f) {
        for (int v = 0; v < 4; ++v) {
            d.positions.insert(d.positions.end(), {fv[f][v][0], fv[f][v][1], fv[f][v][2]});
            d.normals.insert(d.normals.end(), {fn[f][0], fn[f][1], fn[f][2]});
        }
        const unsigned b = unsigned(f * 4);
        d.indices.insert(d.indices.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
    return d;
}

/// The checked picture's byte at (x, y, channel); the caller has bounded x and y.
int byteAt(const Value &pic, std::int64_t x, std::int64_t y, int c)
{
    return pic["rgba"].asBytes()[size_t((y * pic["width"].asInt() + x) * 4 + c)];
}

/// The picture's own consistency: width x height x 4 bytes.
bool wellFormed(Call &c, const Value &pic)
{
    const std::int64_t w = pic["width"].asInt(), h = pic["height"].asInt();
    if (w <= 0 || h <= 0 || std::int64_t(pic["rgba"].asBytes().size()) != w * h * 4) {
        c.fail("the picture is not width x height x 4 bytes");
        return false;
    }
    return true;
}

class Commands {
public:
    explicit Commands(std::shared_ptr<EngineState> s) : st(std::move(s)) {}

    /// The engine, or the call FAILS saying why it is absent.
    Engine *engine(Call &c) const
    {
        if (!st->engine) c.fail("the engine did not start: " + st->bootError);
        return st->engine;
    }
    Scene *scene(Call &c, const Value &h) const
    {
        if (!engine(c)) return nullptr;
        const std::int64_t i = h.asInt();
        if (i < 1 || i > std::int64_t(st->scenes.size()) || !st->scenes[size_t(i - 1)]) {
            c.fail("no scene has the handle " + std::to_string(i));
            return nullptr;
        }
        return st->scenes[size_t(i - 1)];
    }
    View *view(Call &c, const Value &h) const
    {
        if (!engine(c)) return nullptr;
        const std::int64_t i = h.asInt();
        if (i < 1 || i > std::int64_t(st->views.size()) || !st->views[size_t(i - 1)]) {
            c.fail("no view has the handle " + std::to_string(i));
            return nullptr;
        }
        return st->views[size_t(i - 1)];
    }
    /// One lockstep frame: the harness's grid step (the file's header says why), then the frame; then EVERY PENDING
    /// ROUTE DECLARATION JUDGED on it (RouteDeclaration.h: held or broken, each a law of the host's verdict; a product
    /// not live yet waits). False, the call FAILED naming the first broken declaration, when one broke.
    bool frame(Call &c) const
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

    std::shared_ptr<EngineState> st;
};

}  // namespace

void registerEngineCommands(Host &host, Engine *engine, std::string bootError, bool testDoors)
{
    Registry &r = host.registry();
    auto st = std::make_shared<EngineState>();
    st->host = &host;
    st->engine = engine;
    st->bootError = std::move(bootError);
    st->testDoors = testDoors;
    const Commands k(st);
    const PbrParams pbr;
    const GiParams gi;
    const PostFxDesc fx;

    r.add({"engine.scene.create", {{arg("name", Kind::String)}, Kind::Int},
           "A new scene, empty; answers -> scene handle."},
          [k](Call &c) -> Value {
              Engine *e = k.engine(c);
              if (!e) return Value();
              Scene *s = e->createScene(c[0].asString());
              if (!s) return c.refuse(e->lastError());
              k.st->scenes.push_back(s);
              return Value(k.st->scenes.size());
          });
    r.add({"engine.view.createOffscreen",
           {{object("desc", {arg("name", Kind::String), arg("width", Kind::Int), arg("height", Kind::Int),
                             numbers("background", 3, Value(Value::Array{Value(0.0), Value(0.0), Value(0.0)}))})},
            Kind::Int},
           "A new offscreen view of width x height (its background an RGB in 0..1), drawing live (the scripted frames); "
           "answers -> view handle. Bind a scene with engine.view.setScene."},
          [k](Call &c) -> Value {
              Engine *e = k.engine(c);
              if (!e) return Value();
              const std::int64_t w = c[0]["width"].asInt(), h = c[0]["height"].asInt();
              if (w < 1 || h < 1 || w > 16384 || h > 16384) return c.fail("width and height must be 1..16384");
              View *v = e->createOffscreenView(c[0]["name"].asString(), unsigned(w), unsigned(h),
                                               toColour(c[0]["background"]), OffscreenContract::Live);
              if (!v) return c.refuse(e->lastError());
              k.st->views.push_back(v);
              return Value(k.st->views.size());
          });
    r.add({"engine.view.setScene", {{target("view", Kind::Int), object("bind", {arg("scene", Kind::Int)})}, Kind::Bool},
           "The scene the view draws: {scene: scene handle}."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              Scene *s = v ? k.scene(c, c[1]["scene"]) : nullptr;
              if (!s) return Value();
              if (!v->setScene(s)) return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.scene.setAmbient",
           {{target("scene", Kind::Int), object("ambient", {numbers("upper", 3), numbers("lower", 3)})}, Kind::Null},
           "The scene's hemisphere ambient, {upper, lower}: the upper and the lower colour (linear RGB)."},
          [k](Call &c) -> Value {
              if (Scene *s = k.scene(c, c[0])) s->setAmbient(toColour(c[1]["upper"]), toColour(c[1]["lower"]));
              return Value();
          });
    r.add({"engine.mesh.unitCube", {{target("scene", Kind::Int)}, Kind::Int},
           "A unit cube mesh in the scene (24 vertices, positions and per-face normals, no uvs); answers -> mesh id."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const MeshId id = s->createMesh(unitCube());
              if (!id) return c.refuse(k.st->engine->lastError());
              return Value(id);
          });
    r.add({"engine.material.createPbr",
           {{target("scene", Kind::Int),
             object("params", {numbers("albedo", 3, rgb(pbr.albedo)), arg("metalness", Kind::Number, Value(double(pbr.metalness))),
                               arg("roughness", Kind::Number, Value(double(pbr.roughness))),
                               numbers("emissive", 3, rgb(pbr.emissive))})},
            Kind::Int},
           "A PBR material in the scene: albedo and emissive (linear RGB), metalness and perceptual roughness (0..1); "
           "every other parameter at its default. Answers -> material id."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              PbrParams p;
              p.albedo = toColour(c[1]["albedo"]);
              p.metalness = float(c[1]["metalness"].asNumber());
              p.roughness = float(c[1]["roughness"].asNumber());
              p.emissive = toColour(c[1]["emissive"]);
              const MaterialId id = s->createPbrMaterial(p);
              if (!id) return c.refuse(k.st->engine->lastError());
              return Value(id);
          });
    r.add({"engine.node.create", {{target("scene", Kind::Int)}, Kind::Int},
           "A node at the scene's root, at the identity transform; answers -> node id."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const NodeId id = s->createNode();
              if (!id) return c.refuse(k.st->engine->lastError());
              return Value(id);
          });
    r.add({"engine.node.attachMesh",
           {{target("scene", Kind::Int), target("node", Kind::Int),
             object("draw", {arg("mesh", Kind::Int), arg("material", Kind::Int)})},
            Kind::Bool},
           "Draws {mesh: mesh id, material: material id} at the node."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              if (!s->attachMesh(NodeId(c[1].asInt()), MeshId(c[2]["mesh"].asInt()), MaterialId(c[2]["material"].asInt())))
                  return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.node.setTransform",
           {{target("scene", Kind::Int), target("node", Kind::Int),
             object("transform", {numbers("position", 3, vec(Vec3(0, 0, 0))),
                                  numbers("rotation", 4, Value(Value::Array{Value(0.0), Value(0.0), Value(0.0), Value(1.0)})),
                                  numbers("scale", 3, vec(Vec3(1, 1, 1)))})},
            Kind::Null},
           "The node's local transform: position, rotation (a quaternion x, y, z, w) and scale; an absent part is the "
           "identity's."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const Value::Array &q = c[2]["rotation"].asArray();
              s->setNodeTransform(NodeId(c[1].asInt()), toVec3(c[2]["position"]),
                                  Quat(float(q[0].asNumber()), float(q[1].asNumber()), float(q[2].asNumber()),
                                       float(q[3].asNumber())),
                                  toVec3(c[2]["scale"]));
              return Value();
          });
    static const char *const kModes[] = {"off", "vct", "vctPccHybrid"};
    static const char *const kQualities[] = {"low", "medium", "high", "epic"};
    r.add({"engine.gi.set",
           {{target("scene", Kind::Int),
             object("params", {arg("mode", Kind::String), arg("quality", Kind::String, Value(kQualities[int(gi.quality)])),
                               arg("bounces", Kind::Bool, Value(gi.bounces)),
                               objects("cascades", {arg("halfSize", Kind::Number), arg("resolution", Kind::Int),
                                                    arg("stepCells", Kind::Number, Value(0.0))})})},
            Kind::Bool},
           "The scene's global illumination: mode \"off\" | \"vct\" | \"vctPccHybrid\", quality \"low\" | \"medium\" | "
           "\"high\" | \"epic\", the bounces, and the voxel cascades (1..4 of {halfSize, resolution, stepCells}; none: "
           "the quality's table decides). Every other parameter at its default."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              GiParams p;
              const std::string mode = c[1]["mode"].asString(), quality = c[1]["quality"].asString();
              int m = -1, q = -1;
              for (int i = 0; i < 3; ++i) if (mode == kModes[i]) m = i;
              for (int i = 0; i < 4; ++i) if (quality == kQualities[i]) q = i;
              if (m < 0) return c.fail("mode '" + mode + "' is not off | vct | vctPccHybrid");
              if (q < 0) return c.fail("quality '" + quality + "' is not low | medium | high | epic");
              const Value::Array &cs = c[1]["cascades"].asArray();
              if (cs.size() > size_t(kGiTierMaxCascades))
                  return c.fail("at most " + std::to_string(kGiTierMaxCascades) + " cascades");
              p.mode = GiMode(m);
              p.quality = GiQuality(q);
              p.bounces = c[1]["bounces"].asBool();
              p.cascadeCount = int(cs.size());
              for (size_t i = 0; i < cs.size(); ++i)
                  p.cascadeSet[i] = GiParams::GiCascadeDesc{float(cs[i]["halfSize"].asNumber()), int(cs[i]["resolution"].asInt()),
                                                            float(cs[i]["stepCells"].asNumber())};
              if (!s->setGlobalIllumination(p)) return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.view.setPostFx",
           {{target("view", Kind::Int), object("desc", {arg("allowOffscreen", Kind::Bool, Value(fx.allowOffscreen)),
                                                     arg("ssr", Kind::Int, Value(fx.ssr))})},
            Kind::Null},
           "The view's post chain: allowOffscreen (an offscreen view runs the chain: display-encoded bytes) and ssr "
           "(the reflection's tier, 0 off); every other field at its default."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              PostFxDesc d;
              d.allowOffscreen = c[1]["allowOffscreen"].asBool();
              d.ssr = int(c[1]["ssr"].asInt());
              v->setPostFx(d);
              return Value();
          });
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
    r.add({"engine.view.lookAt", {{target("view", Kind::Int), object("pose", {numbers("eye", 3), numbers("target", 3)})},
                                  Kind::Null},
           "The view's camera, {eye, target}: at `eye` looking at `target`, +Y up, the default projection (45 degrees "
           "vertical)."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              CameraDesc d;
              d.position = toVec3(c[1]["eye"]);
              d.orientation = lookAtRotation(d.position, toVec3(c[1]["target"]));
              v->setCamera(d);
              return Value();
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
    r.add({"engine.view.routes", {{target("view", Kind::Int)}, Kind::Object},
           "What the view and its scene ran on the last frame: the view's chain as built (gather, integrate, "
           "reflection: the published record), the scene's products (field, cards, store: live) and paused (GI's "
           "update budget 0)."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              Scene *s = v->scene();
              if (!s) return c.refuse("the view draws no scene");
              const PublishedEngineFrame::ViewFrames row = v->published();
              const GiStatus g = s->published().gi;
              const inspect::SceneEcho *echo = inspect::scene(s);
              return Value(Value::Object{{"gather", Value(row.routeGather)}, {"integrate", Value(row.routeIntegrate)},
                                         {"reflection", Value(row.routeReflect)}, {"field", Value(g.ifdBound)},
                                         {"cards", Value(g.cards.built)}, {"store", Value(!g.cascades.empty())},
                                         {"paused", Value(echo && echo->giUpdateBudget() == 0)}});
          });
    r.add({"engine.view.declareRoutes",
           {{target("view", Kind::Int),
             object("routes", {arg("gather", Kind::Bool, Value(false)), arg("integrate", Kind::Bool, Value(false)),
                               arg("reflection", Kind::Bool, Value(false)), arg("field", Kind::Bool, Value(false)),
                               arg("cards", Kind::Bool, Value(false)), arg("store", Kind::Bool, Value(false)),
                               arg("paused", Kind::Bool, Value(false))})},
            Kind::Null},
           "THE ROW DECLARES THE ROUTE IT MEASURES (URGENT #146; source.testing_rules R16 reads this call): the view's "
           "chain {gather, integrate, reflection} and paused (GI's update budget 0) EXACTLY, the scene's products {field, "
           "cards, store} live if declared; an absent word is false. Judged after EVERY frame the host draws from here on "
           "(engine.frames, readPixels' frame) as the C++ fixtures' requireRoutes judges: the view's published route row "
           "must equal the engine's own (the two doors agree), the chain must hold on each frame judged, a declared product "
           "has 16 frames to come live. Each judgement is a law of the verdict (`ok:` / `FAIL:` with both sides); a broken "
           "one also fails the drawing call by name; a declaration still pending when the script ends FAILS the verdict."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              Scene *s = v->scene();
              if (!s) return c.fail("the view draws no scene: bind one (engine.view.setScene) before declaring its route");
              DeclaredRoutes d;
              d.view = v;
              d.scene = s;
              d.words.gather = c[1]["gather"].asBool();
              d.words.integrate = c[1]["integrate"].asBool();
              d.words.reflection = c[1]["reflection"].asBool();
              d.words.field = c[1]["field"].asBool();
              d.words.cards = c[1]["cards"].asBool();
              d.words.store = c[1]["store"].asBool();
              d.words.paused = c[1]["paused"].asBool();
              k.st->declared.push_back(d);
              return Value();
          });
    // THE PENDING DECLARATIONS, JUDGED AT THE VERDICT (enginetesthelpers.h's failPendingRoutes): a declaration no frame
    // asserted is a row that measured nothing it said - a FAIL by name, never a silent green
    host.atVerdict([st]() {
        for (const DeclaredRoutes &d : st->declared)
            st->host->law(false, "ROUTES view '" + d.view->name() + "': declared {" + routeWordsText(d.words) +
                                     "} and no frame asserted it before the script ended (URGENT #146)");
        st->declared.clear();
    });
    r.add({"engine.view.readPixels",
           {{object("request", {arg("view", Kind::Int), arg("eyes", Kind::String, Value("left")),
                                arg("content", Kind::String, Value("display")),
                                arg("when", Kind::String, Value("nextFrame"))})},
            Kind::Object},
           "A picture, by PICTURE-REQUEST-1's request: the view, its eyes (\"left\": a mono view), its content "
           "(\"display\": RGBA8, the bytes the view shows) and when (\"nextFrame\": one lockstep frame is drawn and its "
           "picture read). Answers {width, height, rgba} (row-major, top-left origin). Stage 1 serves exactly these "
           "values: right/both, radiance and settled are refused by name until PICTURE-REQUEST-1."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]["view"]);
              if (!v) return Value();
              if (c[0]["eyes"].asString() != "left")
                  return c.fail("eyes '" + c[0]["eyes"].asString() + "': a mono view has the left eye only");
              if (c[0]["content"].asString() != "display")
                  return c.fail("content '" + c[0]["content"].asString() + "' is not served (display only, until PICTURE-REQUEST-1)");
              if (c[0]["when"].asString() != "nextFrame")
                  return c.fail("when '" + c[0]["when"].asString() + "' is not served (nextFrame only, until PICTURE-REQUEST-1)");
              if (!k.frame(c)) return Value();
              Image img;
              if (!v->readPixels(img)) return c.refuse(k.st->engine->lastError());
              return Value(Value::Object{{"width", Value(img.width)}, {"height", Value(img.height)},
                                         {"rgba", Value::bytes(std::move(img.rgba))}});
          });
    r.add({"engine.view.hash", {{target(picture("picture"))}, Kind::String},
           "The picture's FNV-1a 64 over its RGBA bytes, as 16 hex digits."},
          [](Call &c) -> Value {
              if (!wellFormed(c, c[0])) return Value();
              unsigned long long h = 14695981039346656037ull;
              for (std::uint8_t b : c[0]["rgba"].asBytes()) {
                  h ^= b;
                  h *= 1099511628211ull;
              }
              char text[17];
              std::snprintf(text, sizeof text, "%016llx", h);
              return Value(text);
          });
    r.add({"engine.view.columnStep",
           {{target(picture("picture")), object("span", {arg("axis", Kind::String), arg("a", Kind::Int), arg("b", Kind::Int),
                                                 arg("from", Kind::Int), arg("to", Kind::Int)})},
            Kind::Number},
           "The mean absolute step, in codes over R, G and B, between column (axis \"column\") or row (\"row\") a and b, "
           "over the other axis' span [from, to)."},
          [](Call &c) -> Value {
              const Value &p = c[0], &sp = c[1];
              if (!wellFormed(c, p)) return Value();
              const bool columns = sp["axis"].asString() == "column";
              if (!columns && sp["axis"].asString() != "row") return c.fail("axis must be \"column\" or \"row\"");
              const std::int64_t across = columns ? p["width"].asInt() : p["height"].asInt();
              const std::int64_t along = columns ? p["height"].asInt() : p["width"].asInt();
              const std::int64_t a = sp["a"].asInt(), b = sp["b"].asInt(), from = sp["from"].asInt(), to = sp["to"].asInt();
              if (a < 0 || b < 0 || a >= across || b >= across || from < 0 || to > along || from > to)
                  return c.fail("the span lies outside the picture");
              double s = 0.0;
              int n = 0;
              for (std::int64_t t = from; t < to; ++t)
                  for (int ch = 0; ch < 3; ++ch, ++n)
                      s += std::fabs(double(columns ? byteAt(p, a, t, ch) - byteAt(p, b, t, ch)
                                                    : byteAt(p, t, a, ch) - byteAt(p, t, b, ch)));
              return Value(n ? s / n : 0.0);
          });
    r.add({"engine.view.mean",
           {{target(picture("picture")), object("rect", {arg("x0", Kind::Int), arg("y0", Kind::Int), arg("x1", Kind::Int),
                                                 arg("y1", Kind::Int)})},
            Kind::Array},
           "The mean of each channel, in codes, over the rectangle [x0, x1) x [y0, y1): [r, g, b, a]."},
          [](Call &c) -> Value {
              const Value &p = c[0], &rc = c[1];
              if (!wellFormed(c, p)) return Value();
              const std::int64_t x0 = rc["x0"].asInt(), y0 = rc["y0"].asInt(), x1 = rc["x1"].asInt(), y1 = rc["y1"].asInt();
              if (x0 < 0 || y0 < 0 || x1 > p["width"].asInt() || y1 > p["height"].asInt() || x0 >= x1 || y0 >= y1)
                  return c.fail("the rectangle is empty or lies outside the picture");
              double sum[4] = {0, 0, 0, 0};
              for (std::int64_t y = y0; y < y1; ++y)
                  for (std::int64_t x = x0; x < x1; ++x)
                      for (int ch = 0; ch < 4; ++ch) sum[ch] += byteAt(p, x, y, ch);
              const double n = double((x1 - x0) * (y1 - y0));
              return Value(Value::Array{Value(sum[0] / n), Value(sum[1] / n), Value(sum[2] / n), Value(sum[3] / n)});
          });
}

}}}  // namespace jahshaka::engine::script
