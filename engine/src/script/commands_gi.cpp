// THE GLOBAL-ILLUMINATION FAMILY (SCRIPT-IN-ENGINE-1): engine.gi.* - a scene's GI.
#include "script/commands.h"

#include <limits>

namespace jahshaka { namespace engine { namespace script {

void registerGiCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const GiParams gi;
    static const char *const kModes[] = {"off", "vct", "vctPccHybrid"};
    static const char *const kQualities[] = {"low", "medium", "high", "epic"};
    r.add({"engine.gi.set",
           {Access::Changes, {target(handle("scene", kScene)),
             object("params", {arg("mode", Kind::String).oneOf({kModes[0], kModes[1], kModes[2]}),
                               arg("quality", Kind::String, Value(kQualities[int(gi.quality)]))
                                   .oneOf({kQualities[0], kQualities[1], kQualities[2], kQualities[3]}),
                               arg("bounces", Kind::Bool, Value(gi.bounces)),
                               // THE GI's OWN CASCADE BOUNDS (PhotonComponent::resolveCascadeTable, PhotonGi.cpp:4858-4893):
                               // at most kGiTierMaxCascades (Types.h:2493); a resolution the engine CLAMPS to 16..256
                               // (giCascadeResolution, Types.h:3396-3400); a halfSize <= 0 drops the WHOLE table to the
                               // quality's (PhotonGi.cpp:4866); a pinned step is clamped to 1..resolution/2 (:4876-4877), so
                               // 0..128 here and the rest in the body
                               objects("cascades", {arg("halfSize", Kind::Number).above(0).in("m"),
                                                    arg("resolution", Kind::Int).range(16, 256).in("cells"),
                                                    arg("stepCells", Kind::Number, Value(0.0)).range(0, 128).in("cells")})
                                   .upTo(unsigned(kGiTierMaxCascades))})},
            Kind::Bool},
           "The scene's global illumination: mode \"off\" | \"vct\" | \"vctPccHybrid\", quality \"low\" | \"medium\" | "
           "\"high\" | \"epic\", the bounces, and the voxel cascades (none: the quality's table decides; else up to 4, "
           "each {halfSize > 0 m, resolution 16..256 cells and a multiple of 8, stepCells 0 = derived or "
           "1..resolution/2}, each one's halfSize and cell (halfSize x 2 / resolution) larger than the one before: the "
           "engine's own law, a table it would round, clamp or drop FAILS by name). Every other parameter at its "
           "default."},
          [k](Call &c) -> Value {
              // THE REST OF THE ENGINE's CASCADE LAW, checked before the engine is asked (resolveCascadeTable would round,
              // clamp or drop the table for the quality's - silently): the caller's mistake, by name
              GiParams p;
              const Value::Array &cs = c[1]["cascades"].asArray();
              p.cascadeCount = int(cs.size());   // (at most kGiTierMaxCascades: declared)
              for (size_t i = 0; i < cs.size(); ++i) {
                  const std::string at = "params.cascades[" + std::to_string(i) + "]";
                  GiParams::GiCascadeDesc &d = p.cascadeSet[i];
                  d.halfSize = float(cs[i]["halfSize"].asNumber());
                  d.resolution = int(cs[i]["resolution"].asInt());   // (16..256 declared: it fits an int)
                  d.stepCells = float(cs[i]["stepCells"].asNumber());
                  if (!(d.halfSize > 0.0f) || d.halfSize > std::numeric_limits<float>::max())
                      return c.fail(at + ".halfSize is no float above 0 (the engine's cascade is float: PhotonGi.cpp:4866 "
                                         "would drop the whole table)");
                  if (d.resolution % 8)
                      return c.fail(at + ".resolution " + std::to_string(d.resolution) + " is not a multiple of 8 (the engine "
                                    "would build " + std::to_string(giCascadeResolution(d.resolution)) +
                                    ": its voxel passes run whole 8^3 bricks)");
                  if (d.stepCells != 0.0f && (d.stepCells < 1.0f || d.stepCells > float(d.resolution) * 0.5f))
                      return c.fail(at + ".stepCells must be 0 (the engine derives it) or 1.." +
                                    std::to_string(d.resolution / 2) + " cells (the engine would clamp it)");
                  const GiParams::GiCascadeDesc *in = i ? &p.cascadeSet[i - 1] : nullptr;
                  if (in && (d.halfSize <= in->halfSize ||
                             d.halfSize * 2.0f / float(d.resolution) <= in->halfSize * 2.0f / float(in->resolution)))
                      return c.fail(at + " does not grow outward: its halfSize and its cell (halfSize x 2 / resolution) "
                                    "must each exceed cascade " + std::to_string(i - 1) + "'s (the engine would drop the "
                                    "whole table for the quality's)");
              }
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              const std::string mode = c[1]["mode"].asString(), quality = c[1]["quality"].asString();
              int m = 0, q = 0;   // (the signature declared the choices: the host refused any other)
              for (int i = 0; i < 3; ++i) if (mode == kModes[i]) m = i;
              for (int i = 0; i < 4; ++i) if (quality == kQualities[i]) q = i;
              p.mode = GiMode(m);
              p.quality = GiQuality(q);
              p.bounces = c[1]["bounces"].asBool();
              if (!s->setGlobalIllumination(p)) return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
}

}}}  // namespace jahshaka::engine::script
