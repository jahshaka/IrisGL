#pragma once
// THE HOSTS' STRUCTURAL COMMANDS, AS MESSAGES (ENGINE-UPGRADE-U3 stage 3).
//
// Every host that edits an engine scene — the mirror's walk, the gizmo and bone overlays, the VR arc — gives its
// commands through here and never by a direct call: each command is a SceneMessage in the engine's pending record,
// submitted the moment it is given (so the order the engine applies them in IS the order every host gave them, across
// hosts), applied at the top of the next frame before the node records, and owning what it carries. A create runs
// under an id the host reserves here (reserveNodeId / reserveMeshId / …), so the host names it at once.
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "jahshaka/engine/Engine.h"

namespace scenecmd {

using jahshaka::engine::Scene;

/// The one door: a message for `scene`, into its engine's pending record (token != 0 asks for a completion).
inline void post(Scene *scene, std::function<bool(Scene &)> fn, unsigned long long token = 0)
{
    if (!scene || !fn) return;
    jahshaka::engine::Engine *engine = scene->engine();
    if (!engine) return;
    jahshaka::engine::FrameInput in;
    jahshaka::engine::FrameInputScene part;
    part.scene = scene;
    part.messages.push_back({std::move(fn), token});
    in.scenes.push_back(std::move(part));
    engine->submitFrame(std::move(in));   // moved: a copy would copy what the message owns (a texture's pixels)
}
inline void post(Scene *scene, std::function<void(Scene &)> fn)
{
    if (!fn) return;
    post(scene, std::function<bool(Scene &)>([f = std::move(fn)](Scene &s) { f(s); return true; }));
}
/// A token no host has used (completions are matched by it).
inline unsigned long long newToken()
{
    static std::atomic<unsigned long long> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

/// A member call as a message's body, its answer the call's own (decision 34: a refusal is its message's): a bool
/// call answers itself, a call that returns an id answers "not 0", a void call is accepted. The arguments are COPIED
/// now (a message owns what it carries; a pointer argument is not this).
template <class Fn, class... A>
std::function<bool(Scene &)> bindCall(Fn fn, A &&...args)
{
    return [fn, tup = std::make_tuple(std::decay_t<A>(std::forward<A>(args))...)](Scene &s) mutable {
        return std::apply([&](auto &...a) -> bool {
            using R = decltype((s.*fn)(a...));
            if constexpr (std::is_void_v<R>) { (s.*fn)(a...); return true; }
            else if constexpr (std::is_same_v<R, bool>) return (s.*fn)(a...);
            else return (s.*fn)(a...) != R{};
        }, tup);
    };
}
/// The member call as an unnamed message: a refusal comes back as a token-0 completion.
template <class Fn, class... A>
void call(Scene *scene, Fn fn, A &&...args)
{
    post(scene, bindCall(fn, std::forward<A>(args)...));
}

inline jahshaka::engine::NodeId newNode(Scene *scene, jahshaka::engine::NodeId parent = 0)
{
    const jahshaka::engine::NodeId id = jahshaka::engine::reserveNodeId();
    post(scene, std::function<void(Scene &)>([parent, id](Scene &s) { s.createNode(parent, id); }));
    return id;
}
inline jahshaka::engine::MeshId newMesh(Scene *scene, jahshaka::engine::MeshData data)
{
    const jahshaka::engine::MeshId id = jahshaka::engine::reserveMeshId();
    post(scene, std::function<void(Scene &)>([id, d = std::move(data)](Scene &s) { s.createMesh(d, id); }));
    return id;
}
inline jahshaka::engine::MeshId newLineMesh(Scene *scene, std::vector<jahshaka::engine::Vec3> points, bool strip)
{
    const jahshaka::engine::MeshId id = jahshaka::engine::reserveMeshId();
    post(scene, std::function<void(Scene &)>([id, pts = std::move(points), strip](Scene &s) { s.createLineMesh(pts, strip, id); }));
    return id;
}
inline jahshaka::engine::MaterialId newUnlitMaterial(Scene *scene, const jahshaka::engine::Colour &c, bool depthTest,
                                                     bool wireframe = false)
{
    const jahshaka::engine::MaterialId id = jahshaka::engine::reserveMaterialId();
    post(scene, std::function<void(Scene &)>([id, c, depthTest, wireframe](Scene &s) {
        s.createUnlitMaterial(c, depthTest, wireframe, id);
    }));
    return id;
}
inline jahshaka::engine::MaterialId newOutlineMaterial(Scene *scene, const jahshaka::engine::Colour &c,
                                                       bool skinnable = false)
{
    const jahshaka::engine::MaterialId id = jahshaka::engine::reserveMaterialId();
    post(scene, std::function<void(Scene &)>([id, c, skinnable](Scene &s) { s.createOutlineMaterial(c, skinnable, id); }));
    return id;
}
inline jahshaka::engine::MaterialId newPbrMaterial(Scene *scene, const jahshaka::engine::PbrParams &p)
{
    const jahshaka::engine::MaterialId id = jahshaka::engine::reserveMaterialId();
    post(scene, std::function<void(Scene &)>([id, p](Scene &s) { s.createPbrMaterial(p, id); }));
    return id;
}
/// THE MESSAGE OWNS ITS PIXELS (U3 stage 3, decision 8): a copy, never a pointer into the host's image.
inline jahshaka::engine::TextureId newTexture(Scene *scene, unsigned w, unsigned h, const unsigned char *rgba,
                                              bool srgb, bool mipmaps = false)
{
    const jahshaka::engine::TextureId id = jahshaka::engine::reserveTextureId();
    std::vector<unsigned char> px;
    if (rgba) px.assign(rgba, rgba + size_t(w) * size_t(h) * 4u);
    post(scene, std::function<void(Scene &)>([id, w, h, px = std::move(px), srgb, mipmaps](Scene &s) {
        s.createTexture(w, h, px.empty() ? nullptr : px.data(), srgb, mipmaps, id);
    }));
    return id;
}
inline jahshaka::engine::TextureId newCubemap(Scene *scene, const jahshaka::engine::TextureId faces[6])
{
    const jahshaka::engine::TextureId id = jahshaka::engine::reserveTextureId();
    const std::array<jahshaka::engine::TextureId, 6> f{ faces[0], faces[1], faces[2], faces[3], faces[4], faces[5] };
    post(scene, std::function<void(Scene &)>([id, f](Scene &s) { s.createCubemap(f.data(), id); }));
    return id;
}
inline jahshaka::engine::TextureId newLoadedTexture(Scene *scene, const std::string &path, bool srgb)
{
    const jahshaka::engine::TextureId id = jahshaka::engine::reserveTextureId();
    post(scene, std::function<void(Scene &)>([id, path, srgb](Scene &s) { s.loadTexture(path, srgb, id); }));
    return id;
}
inline jahshaka::engine::TextureId newDecalTexture(Scene *scene, const std::string &path, jahshaka::engine::DecalMap kind)
{
    const jahshaka::engine::TextureId id = jahshaka::engine::reserveTextureId();
    post(scene, std::function<void(Scene &)>([id, path, kind](Scene &s) { s.loadDecalTexture(path, kind, id); }));
    return id;
}

}  // namespace scenecmd
