// THE ATOM FAMILY (SCRIPT-IN-ENGINE-1 2c, builder Q): engine.atom.* - a scene's visibility-buffer doors. Each is a
// MEASURING ARM - the split off (every item through PBS), the id pass's occlusion off (frustum-only), the false-colour
// Atom view - so each is a TEST DOOR (Access::Test: registered for every host, answering only one started with
// --test-doors; the lead's rule, CODING_RULES 1: a core stage is never turned off, an "off" exists only as a measuring
// arm), and a scene's own setting: only a scene this host made (Commands::settable). (atomDrawStatus is 2d's.)
#include "script/commands.h"
#include "script/records_atom.h"

namespace jahshaka { namespace engine { namespace script {

void registerAtomCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const Param scene = target(handle("scene", kScene));
    r.add({"engine.atom.setDraw", {Access::Test, {scene, object("door", {arg("on", Kind::Bool)})}, Kind::Null},
           "A TEST DOOR, the visibility buffer's measurement door (Scene::setAtomDrawEnabled): off, every item draws through "
           "PBS and every view of the scene rebuilds its chain without the id pass; on (the default) the split. A tool's "
           "A/B, never a mode."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              s->setAtomDrawEnabled(c[1]["on"].asBool());
              return Value();
          });
    r.add({"engine.atom.setOcclusion", {Access::Test, {scene, object("door", {arg("on", Kind::Bool)})}, Kind::Null},
           "A TEST DOOR, the id pass's occlusion (Scene::setAtomOcclusionEnabled): on (the default) every view carrying "
           "the id pass culls against the depth pyramid in two passes, the same picture; off, frustum-only. A tool's A/B, "
           "never a mode."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              s->setAtomOcclusionEnabled(c[1]["on"].asBool());
              return Value();
          });
    r.add({"engine.atom.setView", {Access::Test, {scene, object("picture", {paramOf<AtomView>("view")})}, Kind::Null},
           "A TEST DOOR, THE ATOM VIEW (Scene::setAtomView): the visibility buffer's false-colour picture - \"off\" | "
           "\"triangles\" | \"levels\" | \"buckets\" | \"objects\" | \"density\" - over every view of the scene that "
           "carries the id pass; rebuilds nothing; off is byte-identical to a scene that never had it."},
          [k](Call &c) -> Value {
              AtomView v = AtomView::Off;
              std::string why;
              if (!fromValue(c[1]["view"], v, "picture.view", why)) return c.fail(why);
              Scene *s = k.scene(c, c[0]);
              if (!s || !k.settable(c, s)) return Value();
              s->setAtomView(v);
              return Value();
          });
}

}}}  // namespace jahshaka::engine::script
