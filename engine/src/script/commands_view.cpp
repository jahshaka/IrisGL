// THE VIEW FAMILY (SCRIPT-IN-ENGINE-1): engine.view.* - views, their scene, their whole camera and post chain (the
// tables of records_view.h; lookAt stays: a pose, not a record), size, shadows, samples, enable, asynchronous shaders,
// their routes (the plain reader and the asserting declaration), their pictures (display or radiance) and the frame
// capture. A view is the script's to drive whoever made it (a handed-in one too: Script.h); its setters are no
// engine-wide setting (its handle bounds them).
#include "script/commands.h"
#include "script/records_view.h"

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
    r.add({"engine.view.createOffscreen",
           {Access::Changes, {object("desc", {arg("name", Kind::String), arg("width", Kind::Int).range(1, 16384).in("px"),
                             arg("height", Kind::Int).range(1, 16384).in("px"),
                             numbers("background", 4, toValue(Colour())).asFloat()})},   // (records.h's Colour: 4)
            Kind::Int, kView},
           "A new offscreen view of width x height (its background a colour, r g b a, linear 0..1; absent: black), "
           "drawing live (the scripted frames); answers -> view handle. Bind a scene with engine.view.setScene."},
          [k](Call &c) -> Value {
              Engine *e = k.engine(c);
              if (!e) return Value();
              const std::int64_t w = c[0]["width"].asInt(), h = c[0]["height"].asInt();
              Colour background;
              std::string why;
              if (!fromValue(c[0]["background"], background, "background", why)) return c.fail(why);
              View *v = e->createOffscreenView(c[0]["name"].asString(), unsigned(w), unsigned(h), background,
                                               OffscreenContract::Live);
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
           {Access::Changes, {target(handle("view", kView)), Record<PostFxDesc>::table().param("desc")}, Kind::Null},
           "The view's whole post chain (PostFxDesc; a member left out takes the record's default): an offscreen view "
           "runs the chain only with allowOffscreen (display-encoded bytes); hdrReadback keeps the scene's radiance for "
           "readPixels' content \"radiance\" on every view; ssr is the reflection's tier (0 off)."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              PostFxDesc d;
              std::string why;
              if (!Record<PostFxDesc>::table().fromValue(c[1], d, "desc", why)) return c.fail(why);
              v->setPostFx(d);
              return Value();
          });
    r.add({"engine.view.setCamera",
           {Access::Changes, {target(handle("view", kView)), Record<CameraDesc>::table().param("camera")}, Kind::Null},
           "The view's whole camera (CameraDesc; a member left out takes the record's default): position and "
           "orientation absolute (it looks down its local -Z), the vertical field of view, the clip planes, the "
           "projection, the letterbox, the framing hold and the lens shift."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              CameraDesc d;
              std::string why;
              if (!Record<CameraDesc>::table().fromValue(c[1], d, "camera", why)) return c.fail(why);
              v->setCamera(d);
              return Value();
          });
    r.add({"engine.view.resize",
           {Access::Changes,
            {target(handle("view", kView)),
             object("size", {arg("width", Kind::Int).range(1, 16384).in("px"), arg("height", Kind::Int).range(1, 16384).in("px")})},
            Kind::Null},
           "Asks for the view's new size {width, height}: an offscreen view's target is replaced at once (what it drew "
           "is dropped); the achieved size is the view's published width and height."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              v->resize(unsigned(c[1]["width"].asInt()), unsigned(c[1]["height"].asInt()));
              return Value();
          });
    r.add({"engine.view.setShadows", {Access::Changes, {target(handle("view", kView)), arg("on", Kind::Bool)}, Kind::Null},
           "Shadow maps for the view on or off (lights opt in with castShadows); a change rebuilds its workspace."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              v->setShadows(c[1].asBool());
              return Value();
          });
    r.add({"engine.view.setSampleCount",
           {Access::Changes, {target(handle("view", kView)), arg("samples", Kind::Int).range(1, 16)}, Kind::Null},
           "The view's MSAA sample count (1 = off), rounded down to a power of two; the driver may clamp it - the "
           "achieved count is the view's published sampleCount. A change recreates the target."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              v->setSampleCount(unsigned(c[1].asInt()));
              return Value();
          });
    r.add({"engine.view.setEnabled", {Access::Changes, {target(handle("view", kView)), arg("on", Kind::Bool)}, Kind::Null},
           "Enables or disables the view: a disabled view is skipped by every frame."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              v->setEnabled(c[1].asBool());
              return Value();
          });
    r.add({"engine.view.setAsyncShaders", {Access::Changes, {target(handle("view", kView)), arg("on", Kind::Bool)}, Kind::Null},
           "Asynchronous shaders for the view's own frames: on, a permutation not built yet is built in the background "
           "while the view draws its placeholder; off (the default), a frame compiles what it needs first."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              v->setAsyncShaders(c[1].asBool());
              return Value();
          });
    r.add({"engine.view.isOffscreen", {Access::Reads, {target(handle("view", kView))}, Kind::Bool},
           "Whether the view's target is a texture (an offscreen view), fixed at its creation. The target is a "
           "texture; this is not what a route reads: see engine.view.routes."},
          [k](Call &c) -> Value {
              const View *v = k.view(c, c[0]);
              if (!v) return Value();
              return Value(v->isOffscreen());
          });
    r.add({"engine.view.requestFrameCapture",
           {Access::Changes,
            {target(handle("view", kView)), object("capture", {arg("keepHelpers", Kind::Bool, Value(false)),
                                                                arg("alsoPresented", Kind::Bool, Value(false))})},
            Kind::Bool},
           "Arms a one-shot copy of the NEXT frame the view presents: keepHelpers the frame exactly as presented, else "
           "the same frame without the editor's furniture (alsoPresented: the presented one too). Nothing blocks: "
           "engine.view.frameCaptureState polls it, engine.view.takeFrameCapture takes it. False when it cannot arm."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              if (!v->requestFrameCapture(c[1]["keepHelpers"].asBool(), c[1]["alsoPresented"].asBool()))
                  return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.view.frameCaptureState", {Access::Reads, {target(handle("view", kView))}, Kind::String},
           "Where the view's frame capture stands: \"idle\", \"armed\" (the next frame is copied), \"inFlight\" (its "
           "copy's fence has not signalled) or \"ready\"."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              return toValue(v->frameCaptureState());
          });
    r.add({"engine.view.takeFrameCapture",
           {Access::Changes, {target(handle("view", kView)), object("take", {arg("wait", Kind::Bool, Value(false))})},
            Kind::Object},
           "Takes the view's captured frame as a held picture, {picture: picture handle, width, height} - with "
           "{presented: the same, as presented} when it was armed alsoPresented. wait false answers only once ready; "
           "wait true blocks on the fence of a frame already drawn (it draws none). Null when no capture is ready."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              Image shot, presented;
              if (!v->takeFrameCapture(shot, c[1]["wait"].asBool(), &presented))
                  return c.refuse("no frame capture is ready (engine.view.frameCaptureState)");
              if (!k.roomForPicture(c, shot.rgba.size() + presented.rgba.size())) return Value();   // both, or neither
              Value answer = k.holdPicture(c, std::move(shot));
              if (answer.isNull() || presented.rgba.empty()) return answer;
              Value second = k.holdPicture(c, std::move(presented));
              if (second.isNull()) return Value();
              answer.asObject().emplace_back("presented", std::move(second));
              return answer;
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
              return Value(Value::Object{{"gather", Value(row.routeGather)}, {"integrate", Value(row.routeIntegrate)},
                                         {"reflection", Value(row.routeReflect)}, {"field", Value(g.ifdBound)},
                                         {"cards", Value(g.cards.built)}, {"store", Value(!g.cascades.empty())},
                                         {"paused", Value(g.updateBudget == 0)}});
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
              d.viewId = v->id();   // (both live now: their identities, the guard's - commands.h DeclaredRoutes)
              d.sceneSerial = s->serial();
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
        for (const DeclaredRoutes &d : st->declared) {
            // a view its owner handed in and destroyed is never read (Script.h, WHAT IS HANDED IN): named only while held
            const bool live = st->engine && Engine::isLive(st->engineSerial) &&
                              st->engine->viewPresence(d.view, d.viewId) != Scene::Presence::Absent;
            st->host->law(false, "ROUTES " + (live ? "view '" + d.view->name() + "'" : std::string("a view since destroyed")) +
                                     ": declared {" + routeWordsText(d.words) +
                                     "} and no frame asserted it before the script ended (URGENT #146)");
        }
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
           "(\"display\": RGBA8, the bytes the view shows - answers {picture: picture handle, width, height}; "
           "\"radiance\": the scene's linear radiance in float, before the tonemap, on a view whose post chain asked "
           "hdrReadback - answers {radiance: radiance handle, width, height}) and when (\"nextFrame\": one lockstep frame "
           "is drawn and its picture read). The picture is HELD NATIVELY (the engine.picture.* / engine.radiance.* "
           "reductions read it in place; it lives until its release). eyes \"both\" will answer {left, right}, each "
           "such an answer. Served: left, nextFrame; right/both and settled FAIL by name until PICTURE-REQUEST-1."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]["view"]);
              if (!v) return Value();
              if (c[0]["eyes"].asString() != "left")
                  return c.fail("eyes '" + c[0]["eyes"].asString() + "': a mono view has the left eye only");
              if (c[0]["when"].asString() != "nextFrame")
                  return c.fail("when '" + c[0]["when"].asString() + "' is not served (nextFrame only, until PICTURE-REQUEST-1)");
              if (!k.frame(c)) return Value();
              if (c[0]["content"].asString() == "radiance") {
                  ImageF hdr;
                  if (!v->readPixelsHdr(hdr)) return c.refuse(k.st->engine->lastError());
                  return k.holdRadiance(c, std::move(hdr));
              }
              Image img;
              if (!v->readPixels(img)) return c.refuse(k.st->engine->lastError());
              return k.holdPicture(c, std::move(img));
          });
}

}}}  // namespace jahshaka::engine::script
