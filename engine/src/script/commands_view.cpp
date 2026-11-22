// THE VIEW FAMILY (SCRIPT-IN-ENGINE-1): engine.view.* - views, their scene, camera and post chain, their routes (the
// plain reader and the asserting declaration) and their pictures.
#include "script/commands.h"
#include "jahshaka/engine/EngineInspect.h"

#include <algorithm>
#include <cmath>
#include "jahshaka/engine/Camera.h"
#include "jahshaka/engine/GiStatus.h"
#include "jahshaka/engine/Math.h"
#include "jahshaka/engine/Pictures.h"
#include "jahshaka/engine/PostFx.h"
#include "jahshaka/engine/Published.h"

namespace jahshaka { namespace engine { namespace script {

namespace {

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

}  // namespace

void registerViewCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const PostFxDesc fx;
    r.add({"engine.view.createOffscreen",
           {Access::Changes, {object("desc", {arg("name", Kind::String), arg("width", Kind::Int).range(1, 16384).in("px"),
                             arg("height", Kind::Int).range(1, 16384).in("px"),
                             numbers("background", 3, Value(Value::Array{Value(0.0), Value(0.0), Value(0.0)}))})},
            Kind::Int, kView},
           "A new offscreen view of width x height (its background an RGB in 0..1), drawing live (the scripted frames); "
           "answers -> view handle. Bind a scene with engine.view.setScene."},
          [k](Call &c) -> Value {
              Engine *e = k.engine(c);
              if (!e) return Value();
              const std::int64_t w = c[0]["width"].asInt(), h = c[0]["height"].asInt();
              View *v = e->createOffscreenView(c[0]["name"].asString(), unsigned(w), unsigned(h),
                                               toColour(c[0]["background"]), OffscreenContract::Live);
              if (!v) return c.refuse(e->lastError());
              return c.hold(kView, ownedView(k.st, v));
          });
    r.add({"engine.view.destroy", {Access::Changes, {target(handle("view", kView))}, Kind::Bool},
           "Destroys a view a command made (its scene survives); its handle is stale after. A view the host's owner "
           "handed in is not the script's to destroy: FAILS."},
          [k](Call &c) -> Value {
              const View *v = k.view(c, c[0]);
              if (!v) return Value();
              if (std::find(k.st->ownedViews.begin(), k.st->ownedViews.end(), v) == k.st->ownedViews.end())
                  return c.fail("the view '" + v->name() + "' was handed in by the host's owner: not the script's to destroy");
              return Value(c.registry().release(c[0].asInt()));   // (its deleter destroys it in the engine)
          });
    r.add({"engine.view.setScene", {Access::Changes,
                                    {target(handle("view", kView)), object("bind", {handle("scene", kScene)})}, Kind::Bool},
           "The scene the view draws: {scene: scene handle}."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              Scene *s = v ? k.scene(c, c[1]["scene"]) : nullptr;
              if (!s) return Value();
              if (!v->setScene(s)) return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.view.setPostFx",
           {Access::Changes,
            {target(handle("view", kView)), object("desc", {arg("allowOffscreen", Kind::Bool, Value(fx.allowOffscreen)),
                                                     arg("ssr", Kind::Int, Value(fx.ssr)).range(0, 2)})},
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
    r.add({"engine.view.lookAt", {Access::Changes,
                                  {target(handle("view", kView)), object("pose", {numbers("eye", 3), numbers("target", 3)})},
                                  Kind::Null},
           "The view's camera, {eye, target}: at `eye` looking at `target`, +Y up, the default projection (45 degrees "
           "vertical)."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              CameraDesc d;
              d.position = toVec3(c[1]["eye"]);
              const Vec3 t = toVec3(c[1]["target"]);
              // a direction with no horizontal part (or none at all: eye == target) has no right vector under +Y up:
              // the pose would be NaN (the final read's leftover), so it FAILS by name
              if (t.x - d.position.x == 0.f && t.z - d.position.z == 0.f)
                  return c.fail("eye and target are on one vertical line (or equal): +Y up defines no camera there");
              d.orientation = lookAtRotation(d.position, t);
              v->setCamera(d);
              return Value();
          });
    r.add({"engine.view.routes", {Access::Reads, {target(handle("view", kView))}, Kind::Object},
           "What the view and its scene ran on the last frame: the view's chain as built (gather, integrate, "
           "reflection: the published record), the scene's products (field, cards, store: live) and paused (GI's "
           "update budget 0)."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              Scene *s = v->scene();
              if (!s) return c.refuse("the view draws no scene");
              const ViewFrames row = v->published();
              const GiStatus g = s->published().gi;
              const inspect::SceneEcho *echo = inspect::scene(s);
              return Value(Value::Object{{"gather", Value(row.routeGather)}, {"integrate", Value(row.routeIntegrate)},
                                         {"reflection", Value(row.routeReflect)}, {"field", Value(g.ifdBound)},
                                         {"cards", Value(g.cards.built)}, {"store", Value(!g.cascades.empty())},
                                         {"paused", Value(echo && echo->giUpdateBudget() == 0)}});
          });
    r.add({"engine.view.declareRoutes",
           {Access::Reads, {target(handle("view", kView)),
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
    host.atVerdict([st = k.st]() {
        for (const DeclaredRoutes &d : st->declared)
            st->host->law(false, "ROUTES view '" + d.view->name() + "': declared {" + routeWordsText(d.words) +
                                     "} and no frame asserted it before the script ended (URGENT #146)");
        st->declared.clear();
    });
    // THE PENDING DECLARATIONS, JUDGED AT THE VERDICT (enginetesthelpers.h's failPendingRoutes): a declaration no frame
    // asserted is a row that measured nothing it said - a FAIL by name, never a silent green
    r.add({"engine.view.readPixels",
           {Access::Changes,
            {object("request", {handle("view", kView), arg("eyes", Kind::String, Value("left")).oneOf({"left", "right", "both"}),
                                arg("content", Kind::String, Value("display")).oneOf({"display", "radiance"}),
                                arg("when", Kind::String, Value("nextFrame")).oneOf({"nextFrame", "settled"})})},
            Kind::Object},
           "A picture, by PICTURE-REQUEST-1's request: the view, its eyes (\"left\": a mono view), its content "
           "(\"display\": RGBA8, the bytes the view shows) and when (\"nextFrame\": one lockstep frame is drawn and its "
           "picture read). Answers {picture: picture handle, width, height}: the picture is HELD NATIVELY (the "
           "engine.picture.* reductions read it in place; engine.picture.bytes copies it only when asked; it lives until "
           "engine.picture.release). eyes \"both\" will answer {left, right}, each such an answer. Stage 1 serves exactly "
           "left / display / nextFrame: right/both, radiance and settled FAIL by name until PICTURE-REQUEST-1."},
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
              return k.holdPicture(c, std::move(img));
          });
}

}}}  // namespace jahshaka::engine::script
