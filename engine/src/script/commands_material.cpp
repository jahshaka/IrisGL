// THE MATERIAL FAMILY (SCRIPT-IN-ENGINE-1): engine.material.* and engine.texture.* - a scene's materials over the
// whole PbrParams (its one table: records_material.h, fields_material.cpp), the unlit material, the shading model, a
// node's material swap and a material's texture slots; the scene's textures (made from bytes or loaded from a file)
// and its DECAL IMAGES - a separate kind, `decalTexture`, because they are a separate pool (a slice of the process-wide
// decal atlas: Engine.h Scene::loadDecalTexture, "NOT interchangeable with loadTexture"). What a command makes is
// RECORDED as this host's (noteMade) and removed at the host's end (commands_engine.cpp, THE OWNERSHIP LAW): a decal
// image's slice comes back there (Scene::destroyTexture releases it, refcounted). A LOAD names its own id
// (reserveTextureId): a path the scene already holds is then a second, refcounted id of the same image
// (OgreMaterials.cpp loadTexture, OgreDecals.cpp loadDecalTexture) - never the owner's own id, so the host's end
// destroys only what its commands loaded. Registered by makeEngineCommands (commands_engine.cpp).
#include "script/commands.h"
#include "script/records_material.h"
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Lights.h"
#include "jahshaka/engine/Materials.h"

namespace jahshaka { namespace engine { namespace script {

namespace {
/// The PbrParams a checked argument carries; false (the call FAILED by name) for what the table cannot read.
bool pbrOf(Call &c, const Value &v, PbrParams &out)
{
    std::string why;
    if (Record<PbrParams>::table().fromValue(v, out, "params", why)) return true;
    c.fail(why);
    return false;
}
/// A handle argument that may be none (null or 0): its declared default is null.
Param optionalHandle(std::string name, std::string kind)
{
    Param p = handle(std::move(name), std::move(kind));
    p.fallback = Value();
    return p;
}
/// The enum a checked string argument names (paramOf<E> declared the names: the host refused any other).
template <class E> E enumOf(const Value &v)
{
    E e{};
    std::string why;
    fromValue(v, e, "", why);
    return e;
}
}  // namespace

void registerMaterialCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const Param scene = target(handle("scene", kScene));
    const char *const kAlphaLine = " A colour is four numbers r, g, b, a; the alpha of albedo, emissive, specularColour and "
                                   "fresnelColour is carried and not read by the engine today.";
    r.add({"engine.material.createPbr",
           {Access::Changes, {scene, Record<PbrParams>::table().param("params")}, Kind::Int, kMaterial},
           std::string("A PBR material in the scene from the whole PbrParams (every member at its default unless given; "
                       "its shadingModel builds it in that family). Answers -> material id.") + kAlphaLine},
          [k](Call &c) -> Value {
              PbrParams p;
              if (!pbrOf(c, c[1], p)) return Value();
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const MaterialId id = s->createPbrMaterial(p);
              if (!id) return c.refuse(k.st->engine->lastError());
              noteMade(*k.st, s, MadeKind::Material, id);
              return c.name(kMaterial, id);
          });
    r.add({"engine.material.setPbr",
           {Access::Changes, {scene, target(handle("material", kMaterial)), Record<PbrParams>::table().param("params")},
            Kind::Bool},
           std::string("Re-applies the whole PbrParams to a material - every member but shadingModel, which only "
                       "engine.material.setShadingModel switches (Scene::setPbrMaterial).") + kAlphaLine},
          [k](Call &c) -> Value {
              PbrParams p;
              if (!pbrOf(c, c[2], p)) return Value();
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Material, Call::idOf(c[1]), "change")) return Value();
              if (!s->setPbrMaterial(MaterialId(Call::idOf(c[1])), p)) return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.material.createUnlit",
           {Access::Changes,
            {scene, object("unlit", {numbers("colour", 4).range(0, 1).asFloat(), arg("depthTest", Kind::Bool, Value(true)),
                                     arg("wireframe", Kind::Bool, Value(false))})},
            Kind::Int, kMaterial},
           "A flat, unlit material: colour r, g, b, a (an alpha under 1 blends), depthTest (false: drawn on top of "
           "everything), wireframe (the triangle edges only). Answers -> material id."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const Value::Array &a = c[1]["colour"].asArray();
              const Colour colour(float(a[0].asNumber()), float(a[1].asNumber()), float(a[2].asNumber()), float(a[3].asNumber()));
              const MaterialId id = s->createUnlitMaterial(colour, c[1]["depthTest"].asBool(), c[1]["wireframe"].asBool());
              if (!id) return c.refuse(k.st->engine->lastError());
              noteMade(*k.st, s, MadeKind::Material, id);
              return c.name(kMaterial, id);
          });
    r.add({"engine.material.setShadingModel",
           {Access::Changes,
            {scene, target(handle("material", kMaterial)), object("shading", {paramOf<ShadingModel>("model")})},
            Kind::Bool},
           "Moves a material between the shading families \"lit\" | \"unlit\" | \"distortion\", re-attaching every node "
           "that wears it (Scene::setShadingModel); REFUSED, nothing changed, for unlit on a material a rigged mesh wears."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Material, Call::idOf(c[1]), "change")) return Value();
              if (!s->setShadingModel(MaterialId(Call::idOf(c[1])), enumOf<ShadingModel>(c[2]["model"])))
                  return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.material.assign",
           {Access::Changes, {scene, target(handle("node", kNode)), object("wear", {handle("material", kMaterial)})}, Kind::Bool},
           "Swaps the material of a node that draws a mesh, in place (Scene::setNodeMaterial: the node keeps its mesh); "
           "REFUSED across the lit / unlit / distortion families, on a node with no mesh, or for a normal map the mesh "
           "has no tangents for (engine.node.attachMesh is the answer to each)."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Node, Call::idOf(c[1]), "change")) return Value();
              if (!s->setNodeMaterial(NodeId(Call::idOf(c[1])), MaterialId(Call::idOf(c[2]["material"]))))
                  return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.material.setTexture",
           {Access::Changes,
            {scene, target(handle("material", kMaterial)),
             object("bind", {paramOf<PbrTextureSlot>("slot"), optionalHandle("texture", kTexture)})},
            Kind::Bool},
           "Binds a texture of the scene to a slot of a PBR material (Scene::setPbrTexture); texture null clears the "
           "slot. A decal image is no texture (engine.texture.loadDecal's are decal images)."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Material, Call::idOf(c[1]), "change")) return Value();
              const Value &t = c[2]["texture"];
              if (!s->setPbrTexture(MaterialId(Call::idOf(c[1])), enumOf<PbrTextureSlot>(c[2]["slot"]),
                                    t.isNull() ? TextureId(0) : TextureId(Call::idOf(t))))
                  return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.material.destroy",
           {Access::Changes, {scene, target(handle("material", kMaterial))}, Kind::Bool},
           "Destroys a material of the scene; its handle names nothing after."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Material, Call::idOf(c[1]), "destroy")) return Value();
              if (!s->destroyMaterial(MaterialId(Call::idOf(c[1])))) return c.refuse(k.st->engine->lastError());
              forgetMade(*k.st, s, MadeKind::Material, Call::idOf(c[1]));
              return Value(true);
          });

    // ---- THE TEXTURES
    r.add({"engine.texture.create",
           {Access::Changes,
            {scene, object("image", {arg("width", Kind::Int).range(1, 16384).in("px"), arg("height", Kind::Int).range(1, 16384).in("px"),
                                     arg("rgba", Kind::Bytes), arg("srgb", Kind::Bool), arg("mipmaps", Kind::Bool, Value(false))})},
            Kind::Int, kTexture},
           "A texture of the scene from the script's RGBA8 bytes (width x height x 4, row-major, top-left origin): srgb "
           "for a colour map, false for a data map (normal, roughness, metalness); mipmaps builds the whole chain (an "
           "image seen minified). Answers -> texture id."},
          [k](Call &c) -> Value {
              const std::int64_t w = c[1]["width"].asInt(), h = c[1]["height"].asInt();
              const Value::Bytes &rgba = c[1]["rgba"].asBytes();
              if (std::int64_t(rgba.size()) != w * h * 4)
                  return c.fail("image.rgba holds " + std::to_string(rgba.size()) + " bytes, not width x height x 4 = " +
                                std::to_string(w * h * 4));
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const TextureId id = s->createTexture(unsigned(w), unsigned(h), rgba.data(), c[1]["srgb"].asBool(),
                                                    c[1]["mipmaps"].asBool());
              if (!id) return c.refuse(k.st->engine->lastError());
              noteMade(*k.st, s, MadeKind::Texture, id);
              return c.name(kTexture, id);
          });
    r.add({"engine.texture.load",
           {Access::Files, {scene, object("file", {arg("path", Kind::String), arg("srgb", Kind::Bool)})}, Kind::Int, kTexture},
           "A texture of the scene loaded from an image file (png, jpg, tga, dds ...): srgb for a colour map, false for a "
           "data map. A path the scene already holds answers a NEW id of the same image (refcounted: destroying one id "
           "releases only its own). REFUSED for a file that is not there or will not decode. Answers -> texture id."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const TextureId id = s->loadTexture(c[1]["path"].asString(), c[1]["srgb"].asBool(), reserveTextureId());
              if (!id) return c.refuse(k.st->engine->lastError());
              noteMade(*k.st, s, MadeKind::Texture, id);
              return c.name(kTexture, id);
          });
    r.add({"engine.texture.destroy", {Access::Changes, {scene, target(handle("texture", kTexture))}, Kind::Bool},
           "Destroys a texture of the scene (Scene::destroyTexture); its handle names nothing after."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Texture, Call::idOf(c[1]), "destroy")) return Value();
              if (!s->destroyTexture(TextureId(Call::idOf(c[1])))) return c.refuse(k.st->engine->lastError());
              forgetMade(*k.st, s, MadeKind::Texture, Call::idOf(c[1]));
              return Value(true);
          });
    r.add({"engine.texture.loadDecal",
           {Access::Files,
            {scene, object("file", {arg("path", Kind::String), paramOf<DecalMap>("kind")})},
            Kind::Int, kDecalTexture},
           "A DECAL IMAGE loaded from an image file into the decal atlas of its kind (\"diffuse\" | \"normal\" | "
           "\"emissive\"; resampled to the atlas's slice, aspect kept): what engine.decal.set takes - never a texture. "
           "A path the scene already holds answers a new id of the same slice (refcounted). REFUSED when the atlas is "
           "full or the file will not load. Answers -> decalTexture id."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const TextureId id = s->loadDecalTexture(c[1]["path"].asString(), enumOf<DecalMap>(c[1]["kind"]), reserveTextureId());
              if (!id) return c.refuse(k.st->engine->lastError());
              noteMade(*k.st, s, MadeKind::DecalTexture, id);
              return c.name(kDecalTexture, id);
          });
    r.add({"engine.texture.destroyDecal", {Access::Changes, {scene, target(handle("decal", kDecalTexture))}, Kind::Bool},
           "Destroys a decal image of the scene: its atlas slice is released (refcounted: the last id of a slice frees "
           "it); its handle names nothing after."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::DecalTexture, Call::idOf(c[1]), "destroy")) return Value();
              if (!s->destroyTexture(TextureId(Call::idOf(c[1])))) return c.refuse(k.st->engine->lastError());
              forgetMade(*k.st, s, MadeKind::DecalTexture, Call::idOf(c[1]));
              return Value(true);
          });
}

}}}  // namespace jahshaka::engine::script
