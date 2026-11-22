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
}

}}}  // namespace jahshaka::engine::script
