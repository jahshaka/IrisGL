#pragma once
// THE ENGINE'S COMMANDS, SHARED BY THEIR FAMILIES (SCRIPT-IN-ENGINE-1): the state every family's commands read (the
// engine, its handles, the pending route declarations), the helpers they share, and each family's registration. ONE FILE
// PER FAMILY (commands_<family>.cpp: a lane adding commands adds or edits its family's file, never one shared list);
// commands_engine.cpp says the shape every command follows and registers the families. Private to the engine.
#include "jahshaka/engine/Engine.h"
#include "jahshaka/engine/Script.h"
#include "script/RouteDeclaration.h"

#include <memory>
#include <string>
#include <vector>

namespace jahshaka { namespace engine { namespace script {

/// A route declaration not yet judged held (engine.view.declareRoutes): its view, its scene, the words, the frames since.
struct DeclaredRoutes {
    View *view = nullptr;
    Scene *scene = nullptr;
    RouteWords words;
    int frames = 0;
};

/// AN ENGINE OBJECT A COMMAND MADE by the engine's own id (a node, mesh or material) in `scene`: removed at the host's
/// end (THE OWNERSHIP LAW: a scene the host's owner handed in outlives the host, and what a run made in it must not).
enum class MadeKind { Node, Mesh, Material };
struct MadeId {
    Scene *scene = nullptr;
    MadeKind kind = MadeKind::Node;
    std::uint32_t id = 0;
};

struct EngineState {
    Host *host = nullptr;
    Engine *engine = nullptr;
    std::string bootError;
    bool testDoors = false;
    std::vector<DeclaredRoutes> declared;
    std::size_t pictureBytes = 0;      // the live pictures' bytes (commands_picture.cpp: THE CAP)
    std::size_t pictureBytesCap = 0;   // the cap: the host's option (Host::Options::pictureBytesCap)
    /// THE OWNERSHIP LAW (2b; commands_engine.cpp says it): the ids the commands made, in order; the scenes and views
    /// the commands made (owned: destroyed at their release or the host's end - every other held one was handed in);
    /// and whether the host's end found its engine gone (then nothing calls into it).
    std::vector<MadeId> made;
    /// A scene a command made, and its clock: the serial of the grid steps this host stamped (THE FRAME's PUBLIC DOOR,
    /// Commands::frame: +1 a frame, the scene's one clock writer).
    struct OwnedScene {
        Scene *scene = nullptr;
        unsigned long long stepSerial = 0;
    };
    std::vector<OwnedScene> ownedScenes;
    std::vector<const View *> ownedViews;
    bool engineGone = false;
};

inline Value vec(const Vec3 &v) { return Value(Value::Array{Value(double(v.x)), Value(double(v.y)), Value(double(v.z))}); }
inline Value rgb(const Colour &c) { return Value(Value::Array{Value(double(c.r)), Value(double(c.g)), Value(double(c.b))}); }
inline Vec3 toVec3(const Value &v)
{
    const Value::Array &a = v.asArray();
    return Vec3(float(a[0].asNumber()), float(a[1].asNumber()), float(a[2].asNumber()));
}
inline Colour toColour(const Value &v)
{
    const Value::Array &a = v.asArray();
    return Colour(float(a[0].asNumber()), float(a[1].asNumber()), float(a[2].asNumber()));
}

class Commands {
public:
    explicit Commands(std::shared_ptr<EngineState> s) : st(std::move(s)) {}

    /// The engine, or the call FAILS saying why it is absent.
    Engine *engine(Call &c) const
    {
        if (!st->engine) c.fail("the engine did not start: " + st->bootError);
        return st->engine;
    }
    /// The scene / view a checked handle names (the registry checked its kind and that it is live), or null when
    /// the engine is absent (the call FAILED saying why).
    Scene *scene(Call &c, const Value &h) const { return engine(c) ? c.object<Scene>(h) : nullptr; }
    View *view(Call &c, const Value &h) const { return engine(c) ? c.object<View>(h) : nullptr; }
    /// One lockstep frame: one grid step of every scene this host made, through its FrameInput part (commands_engine.cpp:
    /// THE FRAME's PUBLIC DOOR), then the frame; then EVERY PENDING
    /// ROUTE DECLARATION JUDGED on it (RouteDeclaration.h: held or broken, each a law of the host's verdict; a product
    /// not live yet waits). False, the call FAILED naming the first broken declaration, when one broke.
    bool frame(Call &c) const;
    /// THE PICTURES (commands_picture.cpp): `img` held by the host - {picture: picture handle, width, height} - or null,
    /// the call FAILED by name, past the live pictures' cap; roomForPicture: the cap's check alone (before a copy).
    Value holdPicture(Call &c, Image img) const;
    bool roomForPicture(Call &c, std::size_t bytes) const;

    std::shared_ptr<EngineState> st;
};

/// A SCENE or a VIEW A COMMAND MADE, held OWNED: its handle's release (the destroy command, or the host's end) destroys
/// it in the engine - unless the host's end found the engine gone. Recorded as owned (a handed-in one is not).
std::shared_ptr<Scene> ownedScene(const std::shared_ptr<EngineState> &st, Scene *s);
std::shared_ptr<View> ownedView(const std::shared_ptr<EngineState> &st, View *v);
/// The made id of `kind` recorded (made) or forgotten (destroyed by its command); forgetScene: every one of a scene
/// (destroyed with it).
void noteMade(EngineState &st, Scene *scene, MadeKind kind, std::uint32_t id);
void forgetMade(EngineState &st, const Scene *scene, MadeKind kind, std::uint32_t id);
void forgetScene(EngineState &st, const Scene *scene);
/// The kinds of handle (THE HANDLES, Script.h): held by the host (scene, view) or naming the engine's own id.
constexpr const char *kScene = "scene", *kView = "view", *kNode = "node", *kMesh = "mesh", *kMaterial = "material",
                     *kPicture = "picture";

/// Each family's commands (commands_<family>.cpp), registered by registerEngineCommands (commands_engine.cpp).
void registerSceneCommands(Host &host, const Commands &k);
void registerNodeCommands(Host &host, const Commands &k);
void registerViewCommands(Host &host, const Commands &k);
void registerGiCommands(Host &host, const Commands &k);
void registerPictureCommands(Host &host, const Commands &k);
void registerFrameCommands(Host &host, const Commands &k);
// 2c's split (each family's builder fills its own file; 2d's status family last)
void registerMaterialCommands(Host &host, const Commands &k);
void registerLightCommands(Host &host, const Commands &k);
void registerEffectsCommands(Host &host, const Commands &k);
void registerAtomCommands(Host &host, const Commands &k);
void registerStatusCommands(Host &host, const Commands &k);

}}}  // namespace jahshaka::engine::script
