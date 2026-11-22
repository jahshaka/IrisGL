// THE GLOBAL-ILLUMINATION FAMILY (SCRIPT-IN-ENGINE-1): engine.gi.* - a scene's GI.
#include "script/commands.h"

namespace jahshaka { namespace engine { namespace script {

void registerGiCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const GiParams gi;
    static const char *const kModes[] = {"off", "vct", "vctPccHybrid"};
    static const char *const kQualities[] = {"low", "medium", "high", "epic"};
    r.add({"engine.gi.set",
           {{target(handle("scene", kScene)),
             object("params", {arg("mode", Kind::String).oneOf({kModes[0], kModes[1], kModes[2]}),
                               arg("quality", Kind::String, Value(kQualities[int(gi.quality)]))
                                   .oneOf({kQualities[0], kQualities[1], kQualities[2], kQualities[3]}),
                               arg("bounces", Kind::Bool, Value(gi.bounces)),
                               objects("cascades", {arg("halfSize", Kind::Number).in("m"), arg("resolution", Kind::Int).in("cells"),
                                                    arg("stepCells", Kind::Number, Value(0.0)).in("cells")})})},
            Kind::Bool},
           "The scene's global illumination: mode \"off\" | \"vct\" | \"vctPccHybrid\", quality \"low\" | \"medium\" | "
           "\"high\" | \"epic\", the bounces, and the voxel cascades (1..4 of {halfSize, resolution, stepCells}; none: "
           "the quality's table decides). Every other parameter at its default."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              GiParams p;
              const std::string mode = c[1]["mode"].asString(), quality = c[1]["quality"].asString();
              int m = 0, q = 0;   // (the signature declared the choices: the host refused any other)
              for (int i = 0; i < 3; ++i) if (mode == kModes[i]) m = i;
              for (int i = 0; i < 4; ++i) if (quality == kQualities[i]) q = i;
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
}

}}}  // namespace jahshaka::engine::script
