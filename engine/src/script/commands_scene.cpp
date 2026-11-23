// THE SCENE FAMILY (SCRIPT-IN-ENGINE-1): engine.scene.*, and its meshes - engine.mesh.* (the materials: commands_material.cpp).
#include "script/commands.h"
#include "script/records_scene.h"
#include "jahshaka/engine/FrameInput.h"
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
            {target(handle("scene", kScene)), object("ambient", {numbers("upper", 4), numbers("lower", 4)})}, Kind::Null},
           "The scene's hemisphere ambient, {upper, lower}: the upper and the lower colour (linear r g b a: a Colour is "
           "four numbers; the alpha is carried, not read - the engine reads r g b, OgreScene.cpp setAmbient)."},
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
    r.add({"engine.mesh.create",
           {Access::Changes, {target(handle("scene", kScene)), Record<MeshData>::table().param("mesh")}, Kind::Int, kMesh},
           "A mesh in the scene from FLAT ARRAYS (Scene::createMesh; MeshData): positions and normals xyz per vertex, uvs "
           "uv, tangents xyzw (generated from the uvs when empty), the triangle indices; for GPU skinning four bone indices "
           "and weights per vertex; its LOD levels (lodIndices: level i+1's indices as bytes, uint32 in this machine's "
           "byte order), cards and cluster DAG as a bake carries them. At most 262,144 vertices and 1,048,576 indices; "
           "arrays that do not agree are refused by the engine. Answers -> mesh id."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              MeshData d;
              std::string why;
              if (!Record<MeshData>::table().fromValue(c[1], d, "mesh", why)) return c.fail(why);
              const MeshId id = s->createMesh(d);
              if (!id) return c.refuse(k.st->engine->lastError());
              noteMade(*k.st, s, MadeKind::Mesh, id);
              return c.name(kMesh, id);
          });

    // THE SCENE's SETTINGS (2c, builder P): each acts only on a scene this host made (Commands::settable: a handed-in
    // scene's settings are its owner's). THE FOG and THE SHADOW REQUEST go through the frame's record
    // (Commands::submitScene: FrameInputScene::fog / shadows, applied at the next frame); the sky, the Sky Light's gain,
    // the ambient SH and the LOD bias are the engine's own setters, called now (the ambient SH has no FrameInput field).
    r.add({"engine.scene.setSky",
           {Access::Changes, {target(handle("scene", kScene)), Record<SkyDesc>::table().param("sky")}, Kind::Bool},
           "The scene's whole sky (Scene::setSky; SkyDesc): mode \"noSky\" | \"equirectangular\" | \"cubemap\" | "
           "\"atmosphere\", its images as texture handles (equirect; faces +X -X +Y -Y +Z -Z, none as null), the "
           "atmosphere's dials, the sun disc, the cloud layer (its weather map a texture handle). Idempotent: an equal sky "
           "changes nothing. Refused (the sky unchanged) for faces of unequal size or a compressed face."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              SkyDesc d;
              std::string why;
              if (!Record<SkyDesc>::table().fromValue(c[1], d, "sky", why)) return c.fail(why);
              if (!s->setSky(d)) return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.scene.setEnvironmentLight",
           {Access::Changes, {target(handle("scene", kScene)), paramOf<Colour>("gain").atLeast(0)}, Kind::Null},
           "THE SKY LIGHT's gain on the sky itself (Scene::setEnvironmentLight), four numbers r g b a (linear, >= 0; the "
           "alpha is not read): the drawn sky and the environment captured from it are scaled; 1 is the default, 0 a black "
           "sky. It hands the ambient back to the engine (the capture's)."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              Colour gain;
              std::string why;
              if (!fromValue(c[1], gain, "gain", why)) return c.fail(why);
              s->setEnvironmentLight(gain);
              return Value();
          });
    r.add({"engine.scene.setAmbientSh",
           {Access::Changes, {target(handle("scene", kScene)), numbers("sh", 27).asFloat()}, Kind::Null},
           "The scene's ambient as 9 spherical-harmonic coefficients per channel (Scene::setAmbientSh): 27 numbers, 9 "
           "groups of r g b in the basis order 1, y, z, x, xy, yz, 3z^2 - 1, zx, x^2 - y^2 over world axes; a mean "
           "incident radiance (a uniform white environment of radiance L is sh[0..2] = L, the rest 0). It takes the "
           "ambient from the sky's capture."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              float sh[27];
              for (int i = 0; i < 27; ++i) sh[i] = float(c[1].asArray()[std::size_t(i)].asNumber());
              s->setAmbientSh(sh);
              return Value();
          });
    r.add({"engine.scene.setFog",
           {Access::Changes, {target(handle("scene", kScene)), Record<FogDesc>::table().param("fog")}, Kind::Null},
           "The scene's distance fog with its optional height layer (FogDesc, through the next frame's record): enabled, "
           "colour r g b a (linear; the alpha is not read; the non-analytic skies only), density and heightDensity per "
           "world unit (exp2), heightFalloff, heightLevel. Off is exact."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              FogDesc d;
              std::string why;
              if (!Record<FogDesc>::table().fromValue(c[1], d, "fog", why)) return c.fail(why);
              k.submitScene(c, s, [&d](FrameInputScene &part) { part.fog = d; });
              return Value();
          });
    r.add({"engine.scene.setShadowSettings",
           {Access::Changes, {target(handle("scene", kScene)), Record<ShadowDesc>::table().param("shadows")}, Kind::Null},
           "The scene's shadow request (ShadowDesc, through the next frame's record): a filter \"hard\" | \"soft\" | "
           "\"verySoft\" when hasFilter, a map side in pixels - 0 (no opinion) or 256..8192. The engine's filter and atlas "
           "are one for every scene: the last scene to ask owns them - so it is ENGINE-WIDE: a host for an outside client "
           "refuses it."},
          [k](Call &c) -> Value {
              if (!k.engineWide(c)) return Value();   // (OgreScene::setShadowSettings writes the engine's one filter/atlas)
              const std::int64_t res = c[1]["resolution"].asInt();
              if (res != 0 && res < 256)
                  return c.fail("shadows.resolution " + std::to_string(res) + " is neither 0 (no opinion) nor 256..8192 px "
                                "(the engine would clamp it)");
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              ShadowDesc d;
              std::string why;
              if (!Record<ShadowDesc>::table().fromValue(c[1], d, "shadows", why)) return c.fail(why);
              k.submitScene(c, s, [&d](FrameInputScene &part) { part.shadows = d; });
              return Value();
          });
    r.add({"engine.scene.setLodBias",
           {Access::Changes, {target(handle("scene", kScene)), arg("bias", Kind::Number).atLeast(0).asFloat()}, Kind::Null},
           "The scene's LOD dial (Scene::setLodBias): a multiplier on every view's pixel budget, 1 the reference, larger "
           "coarser, 0 pins every mesh at level 0. Applies at once to the meshes that exist. It acts on the meshes the "
           "stock path draws (their LOD chain: measured 8192 / 512 / 32 triangles at 0 / 1 / 8); an Atom-drawn mesh's cut "
           "is Atom's own pixel-error law - the bias does not reach it."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              s->setLodBias(float(c[1].asNumber()));
              return Value();
          });
}

}}}  // namespace jahshaka::engine::script
