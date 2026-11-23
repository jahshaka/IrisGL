// THE MATERIAL FAMILY (SCRIPT-IN-ENGINE-1): engine.material.* and engine.texture.* - createPbr and destroy (moved here
// from commands_scene.cpp by E0, the batch's base); 2c's builder Q adds setPbr over the whole PbrParams, createUnlit,
// setNodeMaterial, setShadingModel, the textures and the decal images (records_material.h, fields_material.cpp).
// Registered by makeEngineCommands (commands_engine.cpp).
#include "script/commands.h"
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Materials.h"

namespace jahshaka { namespace engine { namespace script {

void registerMaterialCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const PbrParams pbr;
    r.add({"engine.material.createPbr",
           {Access::Changes, {target(handle("scene", kScene)),
             object("params", {numbers("albedo", 3, rgb(pbr.albedo)), arg("metalness", Kind::Number, Value(double(pbr.metalness))).range(0, 1),
                               arg("roughness", Kind::Number, Value(double(pbr.roughness))).range(0, 1),
                               numbers("emissive", 3, rgb(pbr.emissive))})},
            Kind::Int, kMaterial},
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
              noteMade(*k.st, s, MadeKind::Material, id);
              return c.name(kMaterial, id);
          });
    r.add({"engine.material.destroy",
           {Access::Changes, {target(handle("scene", kScene)), target(handle("material", kMaterial))}, Kind::Bool},
           "Destroys a material of the scene; its handle names nothing after."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Material, Call::idOf(c[1]), "destroy")) return Value();
              if (!s->destroyMaterial(MaterialId(Call::idOf(c[1])))) return c.refuse(k.st->engine->lastError());
              forgetMade(*k.st, s, MadeKind::Material, Call::idOf(c[1]));
              return Value(true);
          });
}

}}}  // namespace jahshaka::engine::script
