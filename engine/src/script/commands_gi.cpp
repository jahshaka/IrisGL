// THE GLOBAL-ILLUMINATION FAMILY (SCRIPT-IN-ENGINE-1): engine.gi.* - a scene's GI over the WHOLE GiParams (its table:
// records_gi.h, fields_gi.cpp), the placement pin, the whole relight and the sun's contact shadows. A scene's GI is
// the scene's own setting: only a scene this host made (Commands::settable).
#include "script/commands.h"
#include "script/records_gi.h"

#include <limits>
#include "jahshaka/engine/FrameInput.h"
#include "jahshaka/engine/GiModes.h"
#include "jahshaka/engine/GiParams.h"

namespace jahshaka { namespace engine { namespace script {

namespace {
/// THE GI's PARAMETER: the GiParams table's, with its cascade table - cascadeCount and cascadeSet[kGiTierMaxCascades],
/// the record's form - in the COMMAND's form: one list `cascades` of the used entries, each bounded by the GI's own
/// law (PhotonComponent::resolveCascadeTable, PhotonGi.cpp:4858-4893: at most kGiTierMaxCascades; a resolution the
/// engine clamps to 16..256, giCascadeResolution; a halfSize <= 0 drops the WHOLE table to the quality's, :4866; a
/// pinned step clamped to 1..resolution/2, :4876-4877 - so 0..128 here and the rest in the body).
Param giParam()
{
    Param p = Record<GiParams>::table().param("params");
    std::vector<Param> fields;
    for (Param &f : p.fields) {
        if (f.name == "cascadeSet") continue;
        if (f.name == "cascadeCount") {
            Param cascades = objects("cascades", {arg("halfSize", Kind::Number).above(0).in("m"),
                                                  arg("resolution", Kind::Int).range(16, 256).in("cells"),
                                                  arg("stepCells", Kind::Number, Value(0.0)).range(0, 128).in("cells")})
                                 .upTo(unsigned(kGiTierMaxCascades));
            cascades.fallback = Value(Value::Array{});
            fields.push_back(std::move(cascades));
            continue;
        }
        fields.push_back(std::move(f));
    }
    p.fields = std::move(fields);
    return p;
}
}  // namespace

void registerGiCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const Param scene = target(handle("scene", kScene));
    r.add({"engine.gi.set", {Access::Changes, {scene, giParam()}, Kind::Bool},
           "The scene's global illumination over the WHOLE GiParams (Scene::setGlobalIllumination: built from scratch "
           "every call), every member at the record's default unless given: mode \"off\" | \"vct\" | \"vctPccHybrid\", "
           "quality \"low\" | \"medium\" | \"high\" | \"epic\", the bounces, the probe grid's options, the update budget "
           "(0 = paused), the irradiance field, the card cache... `cascades` is this command's form of the record's "
           "cascadeCount and cascadeSet: the voxel cascades in use (none: the quality's table decides; else up to 4, each "
           "{halfSize > 0 m, resolution 16..256 cells and a multiple of 8, stepCells 0 = derived or 1..resolution/2}, "
           "each one's halfSize and cell (halfSize x 2 / resolution) larger than the one before: the engine's own law, "
           "a table it would round, clamp or drop FAILS by name). probeCaptureSize is 0 (the quality's) or a power of "
           "two in 64..1024. THE TEST LEVERS (testProbeRegionMin / Max, testProbeGridFitMax, cascadeVoxelLod false: "
           "measuring arms) leave their defaults only in a host started with --test-doors."},
          [k](Call &c) -> Value {
              GiParams p;
              std::string why;
              if (!Record<GiParams>::table().fromValue(c[1], p, "params", why)) return c.fail(why);
              // THE TEST LEVERS GiParams carries (GiParams.h: the probe region's pin and fit ceiling, "TEST LEVERS ... nothing
              // a user can reach writes them"; cascadeVoxelLod off, "the A/B"): a measuring arm, never a product door -
              // off its default only in a host started with --test-doors (the lead's rule: an "off" exists only as a
              // measuring arm, CODING_RULES 1)
              const GiParams d;
              const char *lever = !(p.testProbeRegionMin == d.testProbeRegionMin) ? "testProbeRegionMin"
                                  : !(p.testProbeRegionMax == d.testProbeRegionMax) ? "testProbeRegionMax"
                                  : p.testProbeGridFitMax != d.testProbeGridFitMax ? "testProbeGridFitMax"
                                  : p.cascadeVoxelLod != d.cascadeVoxelLod ? "cascadeVoxelLod" : nullptr;
              if (lever && !k.st->testDoors)
                  return c.fail(std::string("params.") + lever + " is a test lever (a measuring arm): off its default only in "
                                "a host started with --test-doors");
              if (p.probeCaptureSize != 0 && (p.probeCaptureSize < 64 || (p.probeCaptureSize & (p.probeCaptureSize - 1))))
                  return c.fail("params.probeCaptureSize " + std::to_string(p.probeCaptureSize) + " is neither 0 nor a power "
                                "of two in 64..1024 (the engine would clamp and round it down)");
              // THE REST OF THE ENGINE's CASCADE LAW, checked before the engine is asked (resolveCascadeTable would round,
              // clamp or drop the table for the quality's - silently): the caller's mistake, by name
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
              if (!s->setGlobalIllumination(p)) return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.gi.setPlacementPin", {Access::Test, {scene, Record<GiPlacementPin>::table().param("pin")}, Kind::Null},
           "THE GI PLACEMENT PIN, a TEST DOOR (a measuring arm: Scene::setGiPlacementPin; never a picture path): while `on`, every "
           "re-centre of cascade i takes its placement from cascadeBuiltCam (3 numbers a cascade, innermost first) "
           "unless cascadeFree[i] is 1, and the irradiance field's re-placement puts its window at fieldOrigin when "
           "`field`; off: the eye's. Applies at the next frame."},
          [k](Call &c) -> Value {
              GiPlacementPin pin;
              std::string why;
              if (!Record<GiPlacementPin>::table().fromValue(c[1], pin, "pin", why)) return c.fail(why);
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              s->setGiPlacementPin(pin);
              return Value();
          });
    r.add({"engine.gi.refreshLighting", {Access::Changes, {scene}, Kind::Bool},
           "THE WHOLE RELIGHT (Scene::refreshGiLighting): every cascade's direct term re-injected over the existing voxels "
           "and every brick's bounces resumed - the store converges to the scene's inputs over the next frames; no "
           "re-voxelisation. REFUSED where the scene has no voxel chain."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              if (!s->refreshGiLighting()) return c.refuse("the scene has no voxel chain to relight (its GI is off or not built)");
              return Value(true);
          });
    r.add({"engine.gi.setSunContact", {Access::Changes, {scene, Record<SunContactDesc>::table().param("contact")}, Kind::Bool},
           "The sun's hard contact shadows (the project's row: SunContactDesc - enabled, range > 0 m, resolution \"auto\" "
           "| \"full\" | \"half\"), through the frame's record (the scene's sunContact part, Scene::setSunContact at the "
           "drain): stored for the next frame, building nothing until a view whose scene resolves it on draws."},
          [k](Call &c) -> Value {
              SunContactDesc d;
              std::string why;
              if (!Record<SunContactDesc>::table().fromValue(c[1], d, "contact", why)) return c.fail(why);
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.submitScene(c, s, [&d](FrameInputScene &part) { part.sunContact = d; })) return Value();
              return Value(true);
          });
}

}}}  // namespace jahshaka::engine::script
