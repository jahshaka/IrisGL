// THE STATUS FAMILY (SCRIPT-IN-ENGINE-1 2d): what the engine PUBLISHED and what its monitor recorded, answered as
// records (records_status.h; one field table each, fields_status.cpp) - the engine's half of the last closed frame
// (engine.published), a scene's (engine.scene.published) and a view's (engine.view.published); the frame monitor
// (engine.monitor.set: ENGINE-WIDE, an outside client refused; popRecords / popEvents: the popped queue, Engine.h's
// fifth door - never requested, never waited for); the submit status, the shader cache's figures, a scene's Atom draw
// status and a view's measured exposure. Every read is the engine's PUBLIC API on the calling thread. An engine id a
// published record carries (a NodeId) is answered as its NUMBER: these records name what the engine drew, not a handle
// a later command takes.
#include "script/commands.h"
#include "script/records_status.h"

namespace jahshaka { namespace engine { namespace script {

namespace {

/// THE SELECTOR (the spec's 2d: one whole conversion of a filled record costs more than ~50 us - GiStatus 126,
/// PublishedEngineFrame 136, CPU, Debug: spikes/script-in-engine-1/r/selector-us.txt): a published command takes
/// {select: [<top-level member>...]} and converts ONLY those members; empty (the default) answers the whole record.
template <class R> Param selectParam()
{
    std::vector<std::string> names;
    for (const Field<R> &f : Record<R>::table().fields()) names.push_back(f.param.name);
    Param s = arg("select", Kind::Array, Value(Value::Array{}));
    s.element = Kind::String;
    s.maxCount = unsigned(names.size());
    s.choices = std::move(names);
    return object("options", {s}, Value(Value::Object{}));
}
/// The record `r` answered whole, or its selected top-level members in the order asked (a name that is no member
/// never arrives: selectParam's choices FAIL it by name at the call).
template <class R> Value selected(Call &c, const R &r, const Value &options)
{
    const Value::Array &names = options["select"].asArray();
    if (names.empty()) return toValue(r, &c);
    Value::Object o;
    for (const Value &n : names) {
        for (const Field<R> &f : Record<R>::table().fields())
            if (f.param.name == n.asString()) o.emplace_back(f.param.name, f.get(r, &c));
    }
    return Value(std::move(o));
}

}  // namespace

void registerStatusCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    r.add({"engine.published", {Access::Reads, {selectParam<PublishedEngineFrame>()}, Kind::Object},
           "The engine's half of the last closed frame (PublishedEngineFrame): its frame and serial, the input "
           "drain, the VR session, the compile counters, the views' rows, the frame errors and refusals, the monitor's "
           "status, the shadow atlas and the render figures. vrJoints is one bytes value a hand (left, right): its "
           "joints as VrPose records in their declared C++ layout, 32 bytes each - valid (1 byte, then 3 of padding), "
           "position (3 floats x y z), rotation (4 floats x y z w) - in this machine's byte order (little-endian on "
           "every target we build). {select: [members]} answers only those top-level members."},
          [k](Call &c) -> Value {
              Engine *e = k.engine(c);
              if (!e) return Value();
              return selected(c, e->published(), c[0]);
          });
    r.add({"engine.view.published",
           {Access::Reads, {target(handle("view", kView)), selectParam<ViewFrames>()}, Kind::Object},
           "The view's row of the last closed frame (ViewFrames): its id and name, the frames presented and blank, "
           "the achieved size and sample count, the generations, the effective post chain it drew with and the "
           "chain's routes as built. A view not yet drawn answers the default row (id 0). {select: [members]} answers only "
           "those top-level members."},
          [k](Call &c) -> Value {
              View *v = k.view(c, c[0]);
              if (!v) return Value();
              return selected(c, v->published(), c[1]);
          });
    r.add({"engine.monitor.set", {Access::Changes, {paramOf<MonitorLevel>("level")}, Kind::Null},
           "Switches the frame monitor (\"review\") or off (\"off\"). ENGINE-WIDE: a host for an outside client "
           "refuses it. Stopping it flushes what waits into the queue: the next popRecords returns it."},
          [k](Call &c) -> Value {
              if (!k.engineWide(c)) return Value();
              Engine *e = k.engine(c);
              if (!e) return Value();
              MonitorLevel level = MonitorLevel::Off;
              std::string why;
              if (!fromValue(c[0], level, "level", why)) return c.fail(why);
              e->setFrameMonitor(level);
              return Value();
          });
    r.add({"engine.monitor.popRecords", {Access::Changes, {}, Kind::Array},
           "Pops the frame monitor's finished records (MonitorRecord, oldest first; the queue is emptied - a record "
           "waits for its GPU samples, so pop across frames). A queue nobody pops loses its oldest (the monitor "
           "status' framesDropped). ENGINE-WIDE (the engine's ONE queue: the owner reads it too): a host for an "
           "outside client refuses it."},
          [k](Call &c) -> Value {
              if (!k.engineWide(c)) return Value();   // (the engine's ONE queue: the owner's capture, Engine.h's fifth door)
              Engine *e = k.engine(c);
              if (!e) return Value();
              std::vector<MonitorRecord> records;
              e->popMonitorRecords(records);
              return toValue(records, &c);
          });
    r.add({"engine.monitor.popEvents", {Access::Changes, {}, Kind::Array},
           "Pops the monitor's discrete events (MonitorEvent, oldest first: GI rebuilds, atlas changes, compiles, "
           "texture loads, VRAM flushes, device loss, the host's own); a full queue drops the NEW event (counted). "
           "ENGINE-WIDE: a host for an outside client refuses it."},
          [k](Call &c) -> Value {
              if (!k.engineWide(c)) return Value();
              Engine *e = k.engine(c);
              if (!e) return Value();
              std::vector<MonitorEvent> events;
              e->popMonitorEvents(events);
              return toValue(events, &c);
          });
    r.add({"engine.frameSubmitStatus", {Access::Reads, {}, Kind::Object},
           "The host's side of the hand-over (FrameSubmitStatus): submits, messages, requests, merged scene parts, "
           "the serial the pending record will apply as, the hand-overs so far."},
          [k](Call &c) -> Value {
              Engine *e = k.engine(c);
              if (!e) return Value();
              return toValue(e->frameSubmitStatus(), &c);
          });
    r.add({"engine.shaderCacheStats", {Access::Reads, {}, Kind::Object},
           "The shader cache's figures (ShaderCacheStats)."},
          [k](Call &c) -> Value {
              Engine *e = k.engine(c);
              if (!e) return Value();
              return toValue(e->shaderCacheStats(), &c);
          });
    r.add({"engine.scene.atomDrawStatus", {Access::Reads, {target(handle("scene", kScene))}, Kind::Object},
           "The scene's Atom draw status (AtomDrawStatus): the cut, the casters, the hidden share."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              return toValue(s->atomDrawStatus(), &c);
          });
    r.add({"engine.view.exposureScale", {Access::Reads, {target(handle("view", kView))}, Kind::Number},
           "What the view's automatic exposure has converged on, as the tonemapper's multiplier (0: nothing to read - "
           "no automatic HDR exposure, or no frame presented by its current workspace). A GPU stall: once a picture, "
           "never a frame."},
          [k](Call &c) -> Value {
              const View *v = k.view(c, c[0]);
              if (!v) return Value();
              return Value(double(v->measuredExposureScale()));
          });
}

}}}  // namespace jahshaka::engine::script
