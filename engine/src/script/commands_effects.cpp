// THE EFFECTS FAMILY (SCRIPT-IN-ENGINE-1 2c, builder Q): engine.decal.*, engine.particles.*, engine.billboards.* and
// engine.planar.* - what a node carries besides its mesh (a projected decal, a simulated particle system, a billboard
// set, a reflection plane) and the scene's planar reflections; their records in records_effects.h, fields_effects.cpp.
// Every command acts on a node THIS HOST MADE in a scene its owner handed in (Commands::mine), or on any node of a
// scene it made. LEAST PRIVILEGE FOR PATHS (RULED 9): a particle system's records carry a PATH beside their other fields
// (ParticleAffectorDesc::colourRampPath), so engine.particles.set is `changes` and REFUSES a path, and
// engine.particles.setColourRamp is the `files` command that sets one - re-applying THIS HOST's own last record of the
// node (Commands::keep / kept, the record E0's state keeps per scene; engine.particles.remove erases it: unkeep).
// Registered by makeEngineCommands (commands_engine.cpp).
#include "script/commands.h"
#include "script/records_effects.h"

#include <cstdio>
#include <vector>

namespace jahshaka { namespace engine { namespace script {

namespace {
/// The record a checked argument carries; false (the call FAILED by name) for what its table cannot read.
template <class R> bool recordOf(Call &c, const Value &v, const char *where, R &out)
{
    std::string why;
    if (Record<R>::table().fromValue(v, out, where, why)) return true;
    c.fail(why);
    return false;
}
/// The enum a checked string argument names (paramOf<E> declared the names: the host refused any other).
template <class E> E enumOf(const Value &v)
{
    E e{};
    std::string why;
    fromValue(v, e, "", why);
    return e;
}
/// A handle argument that may be none (null or 0): its declared default is null.
Param optionalHandle(std::string name, std::string kind)
{
    Param p = handle(std::move(name), std::move(kind));
    p.fallback = Value();
    return p;
}
/// THE BILLBOARD BATCH's MAXIMUM: 65,536 quads a call (setTransforms' batch: one call stays well under a frame's CPU).
constexpr unsigned kBillboards = 65536;
}  // namespace

void registerEffectsCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const Param scene = target(handle("scene", kScene)), node = target(handle("node", kNode));
    Param layerParam = paramOf<BillboardLayer>("layer");
    layerParam.fallback = Value("scene");

    // ---- THE DECAL
    r.add({"engine.decal.set", {Access::Changes, {scene, node, Record<DecalDesc>::table().param("decal")}, Kind::Bool},
           "The node's projected decal (Scene::setDecal): an oriented box down the node's local -Y, width x height (local "
           "X, Z) x depth (local Y), overwriting base colour, metalness and roughness on the surfaces inside it. Its "
           "images are DECAL IMAGES (engine.texture.loadDecal: diffuse required, normal and emissive optional)."},
          [k](Call &c) -> Value {
              DecalDesc d;
              if (!recordOf(c, c[2], "decal", d)) return Value();
              if (!d.diffuse) return c.fail("decal.diffuse is none: a decal projects its diffuse image (engine.texture.loadDecal)");
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Node, Call::idOf(c[1]), "change")) return Value();
              if (!s->setDecal(NodeId(Call::idOf(c[1])), d)) return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.decal.remove", {Access::Changes, {scene, node}, Kind::Bool},
           "The node stops being a decal (Scene::removeDecal); the node stays."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Node, Call::idOf(c[1]), "change")) return Value();
              // (the engine answers false, writing nothing, for a node with no decal: OgreDecals.cpp:501 - the reason is ours)
              if (!s->removeDecal(NodeId(Call::idOf(c[1]))))
                  return c.refuse("node " + std::to_string(Call::idOf(c[1])) + " carries no decal");
              return Value(true);
          });

    // ---- THE PARTICLE SYSTEM
    r.add({"engine.particles.set", {Access::Changes, {scene, node, Record<ParticleSystemDesc>::table().param("system")}, Kind::Bool},
           "The node's simulated particle system (Scene::setParticleSystem): its quota, texture, blend, orientation, "
           "emitters (at least one; an emitter's innerExtents is the hole as a FRACTION 0..0.999 of its extents, x and y "
           "read, z not; a minimum never above its maximum) and affectors (key times ascending); a scalar change applies in place, a topology change rebuilds it. A "
           "colour ramp's IMAGE is a path: engine.particles.setColourRamp sets it (a colourRampPath here FAILS); a ramp "
           "it set stays with its affector while that affector stays a colour ramp."},
          [k](Call &c) -> Value {
              ParticleSystemDesc d;
              if (!recordOf(c, c[2], "system", d)) return Value();
              if (d.emitters.empty()) return c.fail("system.emitters is empty: a particle system emits from at least one");
              // WHAT THE ENGINE WOULD REPAIR SILENTLY, refused by name (OgreParticles.cpp:616-619 swaps a minimum above
              // its maximum; fillInterpStages, :185-197, bumps a key time not after the one before)
              const auto num = [](float x) { char b[32]; std::snprintf(b, sizeof b, "%g", double(x)); return std::string(b); };
              for (size_t i = 0; i < d.emitters.size(); ++i) {
                  const ParticleEmitterDesc &e = d.emitters[i];
                  const std::string at = "system.emitters[" + std::to_string(i) + "].";
                  if (e.velocityMin > e.velocityMax)
                      return c.fail(at + "velocityMin " + num(e.velocityMin) + " is above velocityMax " + num(e.velocityMax));
                  if (e.ttlMin > e.ttlMax)
                      return c.fail(at + "ttlMin " + num(e.ttlMin) + " is above ttlMax " + num(e.ttlMax));
              }
              for (size_t i = 0; i < d.affectors.size(); ++i) {
                  const ParticleAffectorDesc &a = d.affectors[i];
                  const bool colours = a.kind == ParticleAffectorDesc::Kind::ColourKeys;
                  if (!colours && a.kind != ParticleAffectorDesc::Kind::ScaleKeys) continue;
                  const float *t = colours ? a.colourKeyTimes : a.scaleKeyTimes;
                  for (unsigned j = 1; j < a.keyCount; ++j)
                      if (!(t[j] > t[j - 1]))
                          return c.fail("system.affectors[" + std::to_string(i) + "]." + (colours ? "colourKeyTimes[" : "scaleKeyTimes[") +
                                        std::to_string(j) + "] " + num(t[j]) + " is not after key " + std::to_string(j - 1) +
                                        "'s " + num(t[j - 1]) + " (the keys run in ascending time)");
              }
              for (size_t i = 0; i < d.affectors.size(); ++i)
                  if (!d.affectors[i].colourRampPath.empty())
                      return c.fail("system.affectors[" + std::to_string(i) + "].colourRampPath is a path: "
                                    "engine.particles.setColourRamp sets it (a files command)");
              Scene *s = k.scene(c, c[0]);
              const std::uint32_t n = Call::idOf(c[1]);
              if (!s || !k.mine(c, s, MadeKind::Node, n, "change")) return Value();
              if (const ParticleSystemDesc *last = k.kept<ParticleSystemDesc>(s, KeptKind::ParticleSystem, n))
                  for (size_t i = 0; i < d.affectors.size() && i < last->affectors.size(); ++i)
                      if (d.affectors[i].kind == ParticleAffectorDesc::Kind::ColourRamp &&
                          last->affectors[i].kind == ParticleAffectorDesc::Kind::ColourRamp)
                          d.affectors[i].colourRampPath = last->affectors[i].colourRampPath;
              if (!s->setParticleSystem(NodeId(n), d)) return c.refuse(k.st->engine->lastError());
              k.keep(s, KeptKind::ParticleSystem, n, std::move(d));
              return Value(true);
          });
    r.add({"engine.particles.setColourRamp",
           {Access::Files,
            {scene, node, object("ramp", {arg("affector", Kind::Int).range(0, kParticleAffectorsMax - 1), arg("path", Kind::String)})},
            Kind::Bool},
           "The IMAGE of a colour-ramp affector (row 0 sampled over a particle's life): re-applies THIS HOST's own last "
           "engine.particles.set of the node with the path at affector `affector` (which must be a \"colourRamp\"); \"\" "
           "clears it. FAILS on a node this host set no particle system on, and with the engine's words where the engine "
           "does not take the image."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              const std::uint32_t n = Call::idOf(c[1]);
              if (!s || !k.mine(c, s, MadeKind::Node, n, "change")) return Value();
              const ParticleSystemDesc *last = k.kept<ParticleSystemDesc>(s, KeptKind::ParticleSystem, n);
              if (!last)
                  return c.fail("node " + std::to_string(n) + " carries no particle system this host set (engine.particles.set "
                                "first)");
              const std::size_t a = std::size_t(c[2]["affector"].asInt());
              if (a >= last->affectors.size())
                  return c.fail("ramp.affector " + std::to_string(a) + ": the node's system has " +
                                std::to_string(last->affectors.size()) + " affectors");
              if (last->affectors[a].kind != ParticleAffectorDesc::Kind::ColourRamp)
                  return c.fail("ramp.affector " + std::to_string(a) + " is a \"" + toValue(last->affectors[a].kind).asString() +
                                "\" affector, not a \"colourRamp\"");
              ParticleSystemDesc d = *last;
              d.affectors[a].colourRampPath = c[2]["path"].asString();
              // THE ENGINE JUDGES THE FILE (one owner of the law: builder P's e1 makes the setter answer false for a
              // ramp image that is not there, naming it): a false FAILS with the engine's words and keeps no record
              if (!s->setParticleSystem(NodeId(n), d)) return c.fail(k.st->engine->lastError());
              k.keep(s, KeptKind::ParticleSystem, n, std::move(d));
              return Value(true);
          });
    r.add({"engine.particles.remove", {Access::Changes, {scene, node}, Kind::Bool},
           "The node stops being an emitter (Scene::removeParticleSystem; its live particles vanish); the node stays, "
           "and this host's record of its system goes with it (a later setColourRamp FAILS)."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              const std::uint32_t n = Call::idOf(c[1]);
              if (!s || !k.mine(c, s, MadeKind::Node, n, "change")) return Value();
              // every system on a node this host may change was set by this host (mine): no record, no system
              if (!k.kept<ParticleSystemDesc>(s, KeptKind::ParticleSystem, n))
                  return c.fail("node " + std::to_string(n) + " carries no particle system this host set");
              if (!s->removeParticleSystem(NodeId(n)))
                  return c.refuse("node " + std::to_string(n) + "'s particle system was not removed (" + k.st->engine->lastError() + ")");
              k.unkeep(s, KeptKind::ParticleSystem, n);
              return Value(true);
          });

    // ---- THE BILLBOARD SET (externally simulated quads, world-space)
    r.add({"engine.billboards.create",
           {Access::Changes,
            {scene, node,
             object("set", {optionalHandle("texture", kTexture), arg("additive", Kind::Bool, Value(true)),
                            arg("capacity", Kind::Int).range(1, kBillboards), layerParam})},
            Kind::Bool},
           "Creates (or replaces) the node's billboard set (Scene::createBillboardSet): up to `capacity` camera-facing "
           "quads, textured (null: untextured white), additive or alpha-blended, in layer \"scene\" (graded with the "
           "scene) or \"overlay\" (drawn after the post chain, no depth test: an editor helper)."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Node, Call::idOf(c[1]), "change")) return Value();
              const Value &t = c[2]["texture"];
              if (!s->createBillboardSet(NodeId(Call::idOf(c[1])), t.isNull() ? TextureId(0) : TextureId(Call::idOf(t)),
                                         c[2]["additive"].asBool(), unsigned(c[2]["capacity"].asInt()),
                                         enumOf<BillboardLayer>(c[2]["layer"])))
                  return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.billboards.set",
           {Access::Changes,
            {scene, node,
             object("quads", {arg("positions", Kind::Array).upTo(3 * kBillboards).flat(3).asFloat().in("m"),
                              arg("sizes", Kind::Array, Value()).upTo(kBillboards).atLeast(0).asFloat().in("m"),
                              arg("rotations", Kind::Array, Value()).upTo(kBillboards).asFloat().in("radians"),
                              arg("colours", Kind::Array, Value()).upTo(4 * kBillboards).flat(4).asFloat()})},
            Kind::Bool},
           "Replaces the node's billboards with N quads, as FLAT arrays (the batch's form): positions (3 N numbers, "
           "world-space centres), sizes (N, edge length), rotations (N, about the view axis) and colours (4 N, r g b a, "
           "multiplying the texture); a part left out is every quad's default (size 1, rotation 0, white). More quads than "
           "the set's capacity (engine.billboards.create) FAIL with the engine's words, the set keeping what it had."},
          [k](Call &c) -> Value {
              const Value &pos = c[2]["positions"], &sz = c[2]["sizes"], &rot = c[2]["rotations"], &col = c[2]["colours"];
              const std::size_t n = pos.asArray().size() / 3;
              const struct { const Value &v; std::size_t each; const char *name; } parts[] = {
                  {sz, 1, "sizes"}, {rot, 1, "rotations"}, {col, 4, "colours"}};
              for (const auto &p : parts)
                  if (!p.v.isNull() && p.v.asArray().size() != p.each * n)
                      return c.fail(std::string("quads.") + p.name + " holds " + std::to_string(p.v.asArray().size()) +
                                    " numbers, not " + std::to_string(p.each) + " x " + std::to_string(n) + " quads");
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Node, Call::idOf(c[1]), "change")) return Value();
              const auto at = [](const Value &v, std::size_t i) { return float(v.asArray()[i].asNumber()); };
              std::vector<BillboardInstance> quads(n);
              for (std::size_t i = 0; i < n; ++i) {
                  quads[i].position = Vec3(at(pos, 3 * i), at(pos, 3 * i + 1), at(pos, 3 * i + 2));
                  if (!sz.isNull()) quads[i].size = at(sz, i);
                  if (!rot.isNull()) quads[i].rotationRadians = at(rot, i);
                  if (!col.isNull()) quads[i].colour = Colour(at(col, 4 * i), at(col, 4 * i + 1), at(col, 4 * i + 2), at(col, 4 * i + 3));
              }
              // (past the set's capacity the engine answers false naming both - the caller's count: FAILED by its words)
              if (!s->setBillboards(NodeId(Call::idOf(c[1])), quads.data(), quads.size())) return c.fail(k.st->engine->lastError());
              return Value(true);
          });

    // ---- THE PLANAR REFLECTIONS
    r.add({"engine.planar.set", {Access::Changes, {scene, Record<PlanarReflectionParams>::table().param("params")}, Kind::Bool},
           "The scene's planar reflections (Scene::setPlanarReflections): budget 0..8 active planes (0: off, nothing "
           "paid), resolution 64..2048 px a power of two, mips, shadows, hdr, accurate lighting, maxDistance, "
           "background. ENGINE-WIDE as well as the scene's (a host for an outside client refuses it): one scene per "
           "process holds the reflections, and enabling them on one disables them on the last."},
          [k](Call &c) -> Value {
              PlanarReflectionParams p;
              if (!recordOf(c, c[1], "params", p)) return Value();
              if (p.resolution & (p.resolution - 1))
                  return c.fail("params.resolution " + std::to_string(p.resolution) + " is not a power of two (the engine "
                                "would round it down)");
              if (!k.engineWide(c)) return Value();
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              if (!s->setPlanarReflections(p)) return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.planar.setReflector",
           {Access::Changes, {scene, node, object("reflector", {arg("on", Kind::Bool)})}, Kind::Bool},
           "Makes (or un-makes) the node a reflection plane (Scene::setNodePlanarReflector): its mesh must be plate-like; "
           "the plane is its thinnest local axis, the positive side reflects. Remembered while the budget is 0."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.mine(c, s, MadeKind::Node, Call::idOf(c[1]), "change")) return Value();
              if (!s->setNodePlanarReflector(NodeId(Call::idOf(c[1])), c[2]["on"].asBool())) return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
}

}}}  // namespace jahshaka::engine::script
