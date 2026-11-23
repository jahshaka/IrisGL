// THE SCENE FAMILY (SCRIPT-IN-ENGINE-1): engine.scene.*, and its meshes - engine.mesh.* (the materials: commands_material.cpp).
#include "script/commands.h"
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Mesh.h"

#include <algorithm>

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
    r.add({"engine.scene.create", {Access::Changes, {arg("name", Kind::String)}, Kind::Int, kScene},
           "A new scene, empty; answers -> scene handle."},
          [k](Call &c) -> Value {
              Engine *e = k.engine(c);
              if (!e) return Value();
              Scene *s = e->createScene(c[0].asString());
              if (!s) return c.refuse(e->lastError());
              return c.hold(kScene, ownedScene(k.st, s));
          });
    r.add({"engine.scene.destroy", {Access::Changes, {target(handle("scene", kScene))}, Kind::Bool},
           "Destroys a scene a command made, with every node, mesh and material in it (the views showing it stay, "
           "showing nothing); its handle - and every handle into it - is stale after. A scene the host's owner handed "
           "in is not the script's to destroy: FAILS."},
          [k](Call &c) -> Value {
              const Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              if (std::none_of(k.st->ownedScenes.begin(), k.st->ownedScenes.end(),
                               [s](const EngineState::OwnedScene &o) { return o.scene == s; }))
                  return c.fail("the scene '" + s->name() + "' was handed in by the host's owner: not the script's to destroy");
              return Value(c.registry().release(c[0].asInt()));   // (its deleter destroys it in the engine)
          });
    r.add({"engine.scene.setAmbient",
           {Access::Changes,
            {target(handle("scene", kScene)), object("ambient", {numbers("upper", 3), numbers("lower", 3)})}, Kind::Null},
           "The scene's hemisphere ambient, {upper, lower}: the upper and the lower colour (linear RGB)."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (s && k.settable(c, s)) s->setAmbient(toColour(c[1]["upper"]), toColour(c[1]["lower"]));
              return Value();
          });
    r.add({"engine.mesh.unitCube", {Access::Changes, {target(handle("scene", kScene))}, Kind::Int, kMesh},
           "A unit cube mesh in the scene (24 vertices, positions and per-face normals, no uvs); answers -> mesh id."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const MeshId id = s->createMesh(unitCube());
              if (!id) return c.refuse(k.st->engine->lastError());
              noteMade(*k.st, s, MadeKind::Mesh, id);
              return c.name(kMesh, id);
          });
    r.add({"engine.mesh.destroy", {Access::Changes, {target(handle("scene", kScene)), target(handle("mesh", kMesh))}, Kind::Bool},
           "Destroys a mesh of the scene; its handle names nothing after (a node still drawing it stops)."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Mesh, Call::idOf(c[1]), "destroy")) return Value();
              if (!s->destroyMesh(MeshId(Call::idOf(c[1])))) return c.refuse(k.st->engine->lastError());
              forgetMade(*k.st, s, MadeKind::Mesh, Call::idOf(c[1]));
              return Value(true);
          });
}

}}}  // namespace jahshaka::engine::script
