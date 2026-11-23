// THE LIGHT FAMILY (SCRIPT-IN-ENGINE-1 2c, builder P): engine.light.* - a node's light (LightDesc, records_light.h),
// its IES profile, its removal, and the engine's shadow-map resolution.
// LEAST PRIVILEGE FOR PATHS (RULED 9): LightDesc carries a file path (iesProfilePath) beside its non-file fields, so
// engine.light.set (`changes`) REFUSES a non-empty path by name, and engine.light.setIesProfile (`files`) sets the path
// alone - re-applying THIS HOST's own last LightDesc of the node (Commands::keep / kept: the host's record, gone with
// the node, the scene, or engine.light.remove's Commands::unkeep).
#include "script/commands.h"
#include "script/records_light.h"

namespace jahshaka { namespace engine { namespace script {

void registerLightCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    r.add({"engine.light.set",
           {Access::Changes, {target(handle("scene", kScene)), target(handle("node", kNode)),
                              Record<LightDesc>::table().param("light")},
            Kind::Bool},
           "Creates or updates the node's light (Scene::setLight; LightDesc: it shines down the node's -Y). The colour is "
           "four numbers, r g b a (linear; its alpha is not read). light.iesProfilePath must be empty (a path is set by "
           "engine.light.setIesProfile, a `files` command): a profile that command set on the node is KEPT."},
          [k](Call &c) -> Value {
              if (!c[2]["iesProfilePath"].asString().empty())
                  return c.fail("light.iesProfilePath: a file path is set by engine.light.setIesProfile (a `files` "
                                "command), never by engine.light.set");
              Scene *s = k.scene(c, c[0]);
              const std::uint32_t node = Call::idOf(c[1]);
              if (!s || !k.mine(c, s, MadeKind::Node, node, "change")) return Value();
              LightDesc d;
              std::string why;
              if (!Record<LightDesc>::table().fromValue(c[2], d, "light", why)) return c.fail(why);
              if (const LightDesc *was = k.kept<LightDesc>(s, KeptKind::Light, node)) d.iesProfilePath = was->iesProfilePath;
              if (!s->setLight(NodeId(node), d)) return c.refuse(k.st->engine->lastError());
              k.keep(s, KeptKind::Light, node, d);
              return Value(true);
          });
    r.add({"engine.light.setIesProfile",
           {Access::Files, {target(handle("scene", kScene)), target(handle("node", kNode)),
                            object("profile", {arg("path", Kind::String)})},
            Kind::Bool},
           "The node's IES photometric profile {path} (an absolute .ies path; \"\" clears it): re-applies the LightDesc "
           "this host last set on the node with the path. FAILS on a node this host set no light on."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              const std::uint32_t node = Call::idOf(c[1]);
              if (!s || !k.mine(c, s, MadeKind::Node, node, "change")) return Value();
              const LightDesc *was = k.kept<LightDesc>(s, KeptKind::Light, node);
              if (!was)
                  return c.fail("node " + std::to_string(node) + ": this host set no light on it (engine.light.set first)");
              LightDesc d = *was;
              d.iesProfilePath = c[2]["path"].asString();
              if (!s->setLight(NodeId(node), d)) return c.refuse(k.st->engine->lastError());
              k.keep(s, KeptKind::Light, node, d);
              return Value(true);
          });
    r.add({"engine.light.remove",
           {Access::Changes, {target(handle("scene", kScene)), target(handle("node", kNode))}, Kind::Bool},
           "Removes the node's light (Scene::removeLight); the node stays. This host's record of the light goes with it "
           "(a later engine.light.setIesProfile FAILS). Refused when the node carries no light."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              const std::uint32_t node = Call::idOf(c[1]);
              if (!s || !k.mine(c, s, MadeKind::Node, node, "change")) return Value();
              if (!s->removeLight(NodeId(node)))
                  return c.refuse("node " + std::to_string(node) + " carries no light");
              k.unkeep(s, KeptKind::Light, node);
              return Value(true);
          });
    r.add({"engine.light.setShadowResolution",
           {Access::Changes, {arg("pixels", Kind::Int).range(256, 8192).in("px")}, Kind::Null},
           "The shadow maps' side in pixels, 256..8192 (Engine::setShadowResolution: the engine's ONE shadow atlas, for "
           "every scene - a change rebuilds it). ENGINE-WIDE: a host for an outside client refuses it."},
          [k](Call &c) -> Value {
              if (!k.engineWide(c)) return Value();
              Engine *e = k.engine(c);
              if (e) e->setShadowResolution(unsigned(c[0].asInt()));
              return Value();
          });
}

}}}  // namespace jahshaka::engine::script
