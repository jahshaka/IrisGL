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

struct EngineState {
    Host *host = nullptr;
    Engine *engine = nullptr;
    std::string bootError;
    bool testDoors = false;
    std::vector<Scene *> scenes;   // handle - 1
    std::vector<View *> views;     // handle - 1
    std::vector<DeclaredRoutes> declared;
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
    Scene *scene(Call &c, const Value &h) const
    {
        if (!engine(c)) return nullptr;
        const std::int64_t i = h.asInt();
        if (i < 1 || i > std::int64_t(st->scenes.size()) || !st->scenes[size_t(i - 1)]) {
            c.fail("no scene has the handle " + std::to_string(i));
            return nullptr;
        }
        return st->scenes[size_t(i - 1)];
    }
    View *view(Call &c, const Value &h) const
    {
        if (!engine(c)) return nullptr;
        const std::int64_t i = h.asInt();
        if (i < 1 || i > std::int64_t(st->views.size()) || !st->views[size_t(i - 1)]) {
            c.fail("no view has the handle " + std::to_string(i));
            return nullptr;
        }
        return st->views[size_t(i - 1)];
    }
    /// One lockstep frame: the harness's grid step (commands_engine.cpp says why), then the frame; then EVERY PENDING
    /// ROUTE DECLARATION JUDGED on it (RouteDeclaration.h: held or broken, each a law of the host's verdict; a product
    /// not live yet waits). False, the call FAILED naming the first broken declaration, when one broke.
    bool frame(Call &c) const;

    std::shared_ptr<EngineState> st;
};

/// Each family's commands (commands_<family>.cpp), registered by registerEngineCommands (commands_engine.cpp).
void registerSceneCommands(Host &host, const Commands &k);
void registerNodeCommands(Host &host, const Commands &k);
void registerViewCommands(Host &host, const Commands &k);
void registerGiCommands(Host &host, const Commands &k);
void registerPictureCommands(Host &host, const Commands &k);
void registerFrameCommands(Host &host, const Commands &k);

}}}  // namespace jahshaka::engine::script
