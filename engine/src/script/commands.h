#pragma once
// THE ENGINE'S COMMANDS, SHARED BY THEIR FAMILIES (SCRIPT-IN-ENGINE-1): the state every family's commands read (the
// engine, its handles, the pending route declarations), the helpers they share, and each family's registration. ONE FILE
// PER FAMILY (commands_<family>.cpp: a lane adding commands adds or edits its family's file, never one shared list);
// commands_engine.cpp says the shape every command follows and registers the families. Private to the engine.
#include "jahshaka/engine/Engine.h"
#include "jahshaka/engine/Pictures.h"
#include "jahshaka/engine/Script.h"
#include "script/RouteDeclaration.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jahshaka { namespace engine {
struct FrameInputScene;   // (jahshaka/engine/FrameInput.h: a family that submits a part includes it)
namespace script {

/// A route declaration not yet judged held (engine.view.declareRoutes): its view and its scene WITH THEIR IDENTITIES
/// (View::id, Scene::serial: taken at the declaration, while both lived - what the guard asks the engine, never the
/// address alone), the words, the frames since.
struct DeclaredRoutes {
    View *view = nullptr;
    Scene *scene = nullptr;
    NodeId viewId = 0;
    unsigned long long sceneSerial = 0;
    RouteWords words;
    int frames = 0;
};

/// AN ENGINE OBJECT A COMMAND MADE by the engine's own id (a node, mesh, material, texture or decal image): removed at
/// the host's end (THE OWNERSHIP LAW: a scene the host's owner handed in outlives the host, and what a run made in it
/// must not - a decal image's atlas slice included: Scene::destroyTexture releases it, refcounted), and the authority of
/// what this host may change or destroy in a scene it does not own. Its key in a scene: kind << 32 | id.
enum class MadeKind { Node, Mesh, Material, Texture, DecalTexture };
inline std::uint64_t madeKey(MadeKind kind, std::uint32_t id) { return std::uint64_t(kind) << 32 | id; }

/// A RECORD THIS HOST SET ON A NODE, KEPT (RULED 9, least privilege for paths): engine.light.setIesProfile and
/// engine.particles.setColourRamp re-apply THIS HOST's own last LightDesc / ParticleSystemDesc of the node with the
/// path - the record lives here, per scene, keyed kind << 32 | node id; gone with its scene (forgetScene) and with its
/// node (forgetMade of the node).
enum class KeptKind { Light, ParticleSystem };

/// WHAT THIS HOST RECORDED IN ONE SCENE, under the scene's IDENTITY (Scene::serial, read while it lived): the ids its
/// commands made there (madeKey) and the records it kept (KeptKind). A record whose serial is not the live scene's
/// belongs to a dead scene that had its address: it is never read, and the first new note replaces it.
struct SceneRecord {
    unsigned long long serial = 0;
    std::set<std::uint64_t> made;
    std::map<std::uint64_t, std::shared_ptr<const void>> kept;
};

struct EngineState {
    Host *host = nullptr;
    Engine *engine = nullptr;
    /// The engine's identity (Engine::serial): the host's every call and its end ask Engine::isLive of it, never trust
    /// the pointer alone (a dead engine, or another at its address).
    unsigned long long engineSerial = 0;
    std::string bootError;
    bool testDoors = false;
    std::vector<DeclaredRoutes> declared;
    std::size_t pictureBytes = 0;      // the live pictures' bytes (commands_picture.cpp: THE CAP)
    std::size_t pictureBytesCap = 0;   // the cap: the host's option (Host::Options::pictureBytesCap)
    /// THE OWNERSHIP LAW (2b; commands_engine.cpp says it): what the commands made and kept, by scene (SceneRecord);
    /// the scenes and views the commands made (owned: destroyed at their release or the host's end - every other held
    /// one was handed in); and whether the host's end found its engine gone (then nothing calls into it).
    std::map<Scene *, SceneRecord> scenes;
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
/// The live scene's record (Scene::serial read now): a record a dead scene left at its address is replaced.
SceneRecord &sceneRecord(EngineState &st, Scene *scene);

inline Value vec(const Vec3 &v) { return Value(Value::Array{Value(double(v.x)), Value(double(v.y)), Value(double(v.z))}); }
inline Vec3 toVec3(const Value &v)
{
    const Value::Array &a = v.asArray();
    return Vec3(float(a[0].asNumber()), float(a[1].asNumber()), float(a[2].asNumber()));
}
/// A COLOUR IS FOUR NUMBERS (r g b a: records.h's Colour, every family): the parameter declares 4, the registry's
/// count check refuses 3 before this is read.
inline Colour toColour(const Value &v)
{
    const Value::Array &a = v.asArray();
    return Colour(float(a[0].asNumber()), float(a[1].asNumber()), float(a[2].asNumber()), float(a[3].asNumber()));
}

class Commands {
public:
    explicit Commands(std::shared_ptr<EngineState> s) : st(std::move(s)) {}

    /// The engine, or the call FAILS saying why it is absent (it did not start, or it is gone: Engine::isLive).
    Engine *engine(Call &c) const;
    /// The scene / view a checked handle names (the registry checked its kind and that its handle is live), or null -
    /// the call FAILED saying why - when the engine is absent or the engine no longer holds it (one its owner handed in
    /// and destroyed before the host ended: Engine::scenePresence / viewPresence; Script.h, WHAT IS HANDED IN).
    Scene *scene(Call &c, const Value &h) const;
    View *view(Call &c, const Value &h) const;
    /// One lockstep frame: one grid step of every scene this host made, through its FrameInput part (commands_engine.cpp:
    /// THE FRAME's PUBLIC DOOR), then the frame; then EVERY PENDING
    /// ROUTE DECLARATION JUDGED on it (RouteDeclaration.h: held or broken, each a law of the host's verdict; a product
    /// not live yet waits). False, the call FAILED naming the first broken declaration, when one broke.
    bool frame(Call &c) const;
    /// THE PICTURES (commands_picture.cpp): `img` held by the host - {picture: picture handle, width, height} - or null,
    /// the call FAILED by name, past the live pictures' cap; roomForPicture: the cap's check alone (before a copy).
    Value holdPicture(Call &c, Image img) const;
    bool roomForPicture(Call &c, std::size_t bytes) const;
    /// THE RADIANCE PICTURE (a second held kind, `radiance`, bound to ImageF: the view's linear scene radiance in
    /// float, View::readPixelsHdr): held as holdPicture holds, {radiance: radiance handle, width, height}, its bytes
    /// counted under the SAME live-pictures cap.
    Value holdRadiance(Call &c, ImageF img) const;
    /// ONE SCENE SETTING THROUGH THE FRAME's RECORD (FrameInput: a scene's part merges field by field, FrameInput.h): a
    /// part for `s` alone, `fill` setting its one field, submitted for the next frame - false, the call FAILED by name,
    /// when the engine is absent or the scene was handed in (its settings are not this host's: settable).
    bool submitScene(Call &c, Scene *s, const std::function<void(FrameInputScene &)> &fill) const;
    /// AN ENGINE-WIDE SETTING (beyond the scenes and views this host made: an arm, a shadow resolution, the shader
    /// cache): false, the call FAILED by name, in a host that serves an OUTSIDE client (Host::Options::outsideClient).
    bool engineWide(Call &c) const;
    /// THE OWNERSHIP LAW AT A CHANGE (2b's final read L-a; the lead's ruling): a command that CHANGES or DESTROYS a
    /// node, mesh, material, texture or decal image (`verb`: "change" / "destroy") may act on every one of a scene this host made, and in a
    /// scene its owner handed in ONLY on one this host made there - else the call FAILS by name, false. settable: a
    /// command that changes a SCENE's own settings acts only on a scene this host made (a handed-in one FAILS).
    bool mine(Call &c, Scene *s, MadeKind kind, std::uint32_t id, const char *verb) const;
    bool settable(Call &c, const Scene *s) const;
    /// THE KEPT RECORD (KeptKind): `record` kept as this host's last of `kind` on `node` in the live scene `s` (a scene
    /// a command just read); kept answers it - null when this host kept none there (or it went with its node or scene).
    template <class T> void keep(Scene *s, KeptKind kind, std::uint32_t node, T record) const
    {
        sceneRecord(*st, s).kept[std::uint64_t(kind) << 32 | node] = std::make_shared<const T>(std::move(record));
    }
    template <class T> const T *kept(const Scene *s, KeptKind kind, std::uint32_t node) const
    {
        const auto it = st->scenes.find(const_cast<Scene *>(s));
        if (it == st->scenes.end() || it->second.serial != s->serial()) return nullptr;
        const auto k = it->second.kept.find(std::uint64_t(kind) << 32 | node);
        return k == it->second.kept.end() ? nullptr : static_cast<const T *>(k->second.get());
    }
    /// ...and ERASED when the thing it records goes but its node stays (engine.light.remove, engine.particles.remove:
    /// a later setIesProfile / setColourRamp then FAILS - "a node this host set no light on"). Serial-checked as kept:
    /// a record a dead scene left at this address is not this scene's to erase.
    void unkeep(const Scene *s, KeptKind kind, std::uint32_t node) const
    {
        const auto it = st->scenes.find(const_cast<Scene *>(s));
        if (it == st->scenes.end() || it->second.serial != s->serial()) return;
        it->second.kept.erase(std::uint64_t(kind) << 32 | node);
        if (it->second.made.empty() && it->second.kept.empty()) st->scenes.erase(it);
    }

    std::shared_ptr<EngineState> st;
};

/// A SCENE or a VIEW A COMMAND MADE, held OWNED: its handle's release (the destroy command, or the host's end) destroys
/// it in the engine - unless the host's end found the engine gone. Recorded as owned (a handed-in one is not).
std::shared_ptr<Scene> ownedScene(const std::shared_ptr<EngineState> &st, Scene *s);
std::shared_ptr<View> ownedView(const std::shared_ptr<EngineState> &st, View *v);
/// The made id of `kind` recorded (made) or forgotten (destroyed by its command: a node takes its kept records with
/// it); forgetScene: every made id and kept record of a scene (destroyed with it).
void noteMade(EngineState &st, Scene *scene, MadeKind kind, std::uint32_t id);
void forgetMade(EngineState &st, Scene *scene, MadeKind kind, std::uint32_t id);
void forgetScene(EngineState &st, Scene *scene);
/// The kinds of handle (THE HANDLES, Script.h): held by the host (scene, view, picture, radiance) or naming the engine's
/// own id (node, mesh, material, texture, decalTexture - a decal image is its own pool: never a texture).
constexpr const char *kScene = "scene", *kView = "view", *kNode = "node", *kMesh = "mesh", *kMaterial = "material",
                     *kPicture = "picture", *kTexture = "texture", *kDecalTexture = "decalTexture",
                     *kRadiance = "radiance";

/// THE ENGINE's COMMANDS ON A HOST (registerEngineCommands, Script.h, is this and nothing else): the state, the host's
/// end, the kinds bound and checked, the test doors' switch, and every family's commands - answering the state, so a
/// test's own commands reach it (script.commands' fixture).
Commands makeEngineCommands(Host &host, Engine *engine, std::string bootError, bool testDoors);

/// Each family's commands (commands_<family>.cpp), registered by makeEngineCommands (commands_engine.cpp).
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
void registerGiReadCommands(Host &host, const Commands &k);

}}}  // namespace jahshaka::engine::script
