// THE SCENE FAMILY (SCRIPT-IN-ENGINE-1): engine.scene.*, and what a scene holds - engine.mesh.*, engine.material.*.
#include "script/commands.h"

namespace jahshaka { namespace engine { namespace script {

namespace {

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

}  // namespace

void registerSceneCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const PbrParams pbr;
    r.add({"engine.scene.create", {Access::Changes, {arg("name", Kind::String)}, Kind::Int, kScene},
           "A new scene, empty; answers -> scene handle."},
          [k](Call &c) -> Value {
              Engine *e = k.engine(c);
              if (!e) return Value();
              Scene *s = e->createScene(c[0].asString());
              if (!s) return c.refuse(e->lastError());
              return c.hold(kScene, unowned(s));
          });
    r.add({"engine.scene.setAmbient",
           {Access::Changes,
            {target(handle("scene", kScene)), object("ambient", {numbers("upper", 3), numbers("lower", 3)})}, Kind::Null},
           "The scene's hemisphere ambient, {upper, lower}: the upper and the lower colour (linear RGB)."},
          [k](Call &c) -> Value {
              if (Scene *s = k.scene(c, c[0])) s->setAmbient(toColour(c[1]["upper"]), toColour(c[1]["lower"]));
              return Value();
          });
    r.add({"engine.mesh.unitCube", {Access::Changes, {target(handle("scene", kScene))}, Kind::Int, kMesh},
           "A unit cube mesh in the scene (24 vertices, positions and per-face normals, no uvs); answers -> mesh id."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const MeshId id = s->createMesh(unitCube());
              if (!id) return c.refuse(k.st->engine->lastError());
              return c.name(kMesh, id);
          });
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
              return c.name(kMaterial, id);
          });
}

}}}  // namespace jahshaka::engine::script
