// THE NODE FAMILY (SCRIPT-IN-ENGINE-1): engine.node.* - a scene's nodes, by the engine's own ids.
#include "script/commands.h"

namespace jahshaka { namespace engine { namespace script {

void registerNodeCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    r.add({"engine.node.create", {Access::Changes, {target(handle("scene", kScene))}, Kind::Int, kNode},
           "A node at the scene's root, at the identity transform; answers -> node id."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const NodeId id = s->createNode();
              if (!id) return c.refuse(k.st->engine->lastError());
              return c.name(kNode, id);
          });
    r.add({"engine.node.attachMesh",
           {Access::Changes, {target(handle("scene", kScene)), target(handle("node", kNode)),
             object("draw", {handle("mesh", kMesh), handle("material", kMaterial)})},
            Kind::Bool},
           "Draws {mesh: mesh id, material: material id} at the node."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              if (!s->attachMesh(NodeId(Call::idOf(c[1])), MeshId(Call::idOf(c[2]["mesh"])),
                                 MaterialId(Call::idOf(c[2]["material"]))))
                  return c.refuse(k.st->engine->lastError());
              return Value(true);
          });
    r.add({"engine.node.setTransform",
           {Access::Changes, {target(handle("scene", kScene)), target(handle("node", kNode)),
             object("transform", {numbers("position", 3, vec(Vec3(0, 0, 0))),
                                  numbers("rotation", 4, Value(Value::Array{Value(0.0), Value(0.0), Value(0.0), Value(1.0)})),
                                  numbers("scale", 3, vec(Vec3(1, 1, 1)))})},
            Kind::Null},
           "The node's local transform: position, rotation (a quaternion x, y, z, w) and scale; an absent part is the "
           "identity's."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const Value::Array &q = c[2]["rotation"].asArray();
              s->setNodeTransform(NodeId(Call::idOf(c[1])), toVec3(c[2]["position"]),
                                  Quat(float(q[0].asNumber()), float(q[1].asNumber()), float(q[2].asNumber()),
                                       float(q[3].asNumber())),
                                  toVec3(c[2]["scale"]));
              return Value();
          });
    // THE FLAT BATCH (2a a4): N nodes' transforms in ONE call, each part a flat array of numbers (2.2 us an element
    // against 7.2 for an array of objects: the S9 bench). THE MAXIMUM, 65,536 nodes: one call stays under ~0.15 s.
    constexpr unsigned kBatch = 65536;
    r.add({"engine.node.setTransforms",
           {Access::Changes,
            {target(handle("scene", kScene)),
             object("transforms", {handles("nodes", kNode).upTo(kBatch),
                                   arg("positions", Kind::Array, Value()).upTo(3 * kBatch).flat(3).asFloat(),
                                   arg("rotations", Kind::Array, Value()).upTo(4 * kBatch).flat(4).asFloat(),
                                   arg("scales", Kind::Array, Value()).upTo(3 * kBatch).flat(3).asFloat()})},
            Kind::Null},
           "The local transforms of N nodes in one call, as engine.node.setTransform sets one: positions (3 N numbers), "
           "rotations (4 N: quaternions x, y, z, w) and scales (3 N), node i's at 3 i / 4 i / 3 i. A part left out is "
           "the identity's for every node. Every node and every number is checked before any node moves."},
          [k](Call &c) -> Value {
              const Value::Array &nodes = c[1]["nodes"].asArray();
              const Value &pos = c[1]["positions"], &rot = c[1]["rotations"], &scl = c[1]["scales"];
              const struct { const Value &v; std::size_t stride; const char *name; } parts[] = {
                  {pos, 3, "positions"}, {rot, 4, "rotations"}, {scl, 3, "scales"}};
              for (const auto &p : parts)
                  if (!p.v.isNull() && p.v.asArray().size() != p.stride * nodes.size())
                      return c.fail(std::string("transforms.") + p.name + " holds " + std::to_string(p.v.asArray().size()) +
                                    " numbers, not " + std::to_string(p.stride) + " x " + std::to_string(nodes.size()) +
                                    " nodes");
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const auto at = [](const Value &v, std::size_t i) { return float(v.asArray()[i].asNumber()); };
              for (std::size_t i = 0; i < nodes.size(); ++i)
                  s->setNodeTransform(NodeId(Call::idOf(nodes[i])),
                                      pos.isNull() ? Vec3(0, 0, 0) : Vec3(at(pos, 3 * i), at(pos, 3 * i + 1), at(pos, 3 * i + 2)),
                                      rot.isNull() ? Quat() : Quat(at(rot, 4 * i), at(rot, 4 * i + 1), at(rot, 4 * i + 2),
                                                                   at(rot, 4 * i + 3)),
                                      scl.isNull() ? Vec3(1, 1, 1) : Vec3(at(scl, 3 * i), at(scl, 3 * i + 1), at(scl, 3 * i + 2)));
              return Value();
          });
}

}}}  // namespace jahshaka::engine::script
