#pragma once
// Jahshaka's engine abstraction.
//
// This is THE boundary between the application and the 3D engine. Studio talks
// only to these types. No Ogre type, header or symbol appears here — swapping the
// backend must not touch a single file under src/.
//
// Interface derived from what Studio DOES, not from what any engine offers.
//
// THREAD AFFINITY — no exceptions: every call on Engine, Scene and View, including
// destruction, must happen on the thread that called Engine::create(). The backend
// owns a single device and is not internally synchronised. Background work
// (thumbnails, imports) posts to that thread; it never calls in directly.
//
// ERRORS: no backend exception ever escapes this boundary. A failing call returns
// null/false and the reason is available from Engine::lastError() until the next
// failing call overwrites it.
#include <memory>
#include <string>
#include "Types.h"

namespace jahshaka { namespace engine {

class Scene;
class View;

/// A renderable scene. Views draw it; several Views may share one, or each may own one.
/// Owned by the Engine: destroy with Engine::destroyScene().
class Scene {
public:
    virtual ~Scene() = default;
    virtual const std::string &name() const = 0;
    virtual void        setAmbient(const Colour &upper, const Colour &lower) = 0;
    /// Linear distance fog on lit (PBR) surfaces, matching the legacy renderer:
    /// mix(surface, colour, clamp((eyeDistance - start) / (end - start), 0, 1)).
    /// Unlit overlays (gizmos, wires, billboards) and the sky are never fogged.
    /// Off by default; cheap to call every frame (no shader recompilation).
    virtual void        setFog(bool enabled, const Colour &colour, float start, float end) = 0;
    /// Textured sky behind everything: an equirectangular (lat-long) image, or a
    /// cubemap texture. SkyMode::NoSky removes it (the View's background shows).
    virtual bool        setSky(SkyMode, TextureId) = 0;
    /// Cubemap sky from six face textures, in the order +X, -X, +Y, -Y, +Z, -Z.
    /// Also feeds environment reflections (IBL) from the same faces.
    virtual bool        setSkyCubemap(const TextureId faces[6]) = 0;
    /// Environment reflections (IBL) WITHOUT touching the sky geometry: six square
    /// face textures (+X, -X, +Y, -Y, +Z, -Z, all the same size) become the mipped
    /// reflection cubemap every PBR material samples. This is how equirectangular
    /// and CPU-baked skies (gradient, realistic) get the reflections cubemap skies
    /// already have — the host resamples its equirect image into six faces and
    /// pushes them here. Passing six zero ids clears the reflections. The face
    /// textures are copied; the caller may destroy them afterwards.
    virtual bool        setSkyReflection(const TextureId faces[6]) = 0;
    /// Removes a node and everything it uniquely owns (mesh, material). Unknown or
    /// already-removed ids are ignored and return false. Children are NOT removed;
    /// they are re-parented to the scene root.
    virtual bool        removeNode(NodeId) = 0;

    // ---- Hierarchy and transforms (VIEWPORT_MIGRATION_PLAN.md step 2) ----
    /// An empty transform node under `parent` (0 = the scene root).
    virtual NodeId      createNode(NodeId parent = 0) = 0;
    virtual bool        setNodeParent(NodeId, NodeId parent) = 0;
    /// Absolute LOCAL transform (relative to the parent). The document owns the
    /// numbers; the engine composes the hierarchy.
    virtual void        setNodeTransform(NodeId, const Vec3 &position, const Quat &rotation,
                                         const Vec3 &scale) = 0;
    /// Hides the node and its subtree.
    virtual void        setNodeVisible(NodeId, bool) = 0;

    // ---- Meshes and materials (step 3/4) ----
    /// Uploads geometry. Returns 0 on invalid data (lastError()).
    virtual MeshId      createMesh(const MeshData &) = 0;
    virtual bool        destroyMesh(MeshId) = 0;
    /// Rewrites the vertex positions (and, when non-empty, normals) of a mesh
    /// created with MeshData::dynamic — the CPU-skinning path: the host computes
    /// skinned vertices per frame and pushes them here. positions is xyz per
    /// vertex and must match the mesh's vertex count; normals likewise or empty
    /// to keep the current ones. Tangents and uvs keep their created values.
    /// Bounds are recomputed so culling stays correct. False (lastError()) for
    /// unknown or non-dynamic meshes or size mismatches.
    virtual bool        updateMeshVertices(MeshId, const std::vector<float> &positions,
                                           const std::vector<float> &normals) = 0;
    virtual MaterialId  createPbrMaterial(const PbrParams &) = 0;
    virtual bool        setPbrMaterial(MaterialId, const PbrParams &) = 0;
    virtual bool        destroyMaterial(MaterialId) = 0;
    /// Makes the node render `mesh` with `material`. A node renders at most one mesh;
    /// attaching again replaces it. Mesh and material may be shared across nodes and
    /// survive the node.
    virtual bool        attachMesh(NodeId, MeshId, MaterialId) = 0;
    virtual bool        detachMesh(NodeId) = 0;

    // ---- Textures (step 4b): image files on disk, shared across materials ----
    /// Loads an image file (png/jpg/tga/dds...). `srgb` for colour maps (albedo,
    /// emissive); false for data maps (normal, roughness, metalness). The same path
    /// loaded twice returns the same id. 0 on failure (lastError()).
    virtual TextureId   loadTexture(const std::string &path, bool srgb) = 0;
    /// A texture from RGBA8 pixels in memory (top-left origin, width*height*4 bytes).
    virtual TextureId   createTexture(unsigned width, unsigned height, const unsigned char *rgba, bool srgb) = 0;
    virtual bool        destroyTexture(TextureId) = 0;
    /// Binds (or, with 0, clears) a texture slot on a PBR material.
    virtual bool        setPbrTexture(MaterialId, PbrTextureSlot, TextureId) = 0;

    // ---- Overlay primitives (step 8): gizmos, light wires, animation paths ----
    /// Flat colour, unlit. With depthTest=false it draws on top of everything —
    /// what gizmo handles need. Alpha < 1 blends.
    /// `wireframe` draws only the triangle edges — the selection outline uses it.
    virtual MaterialId  createUnlitMaterial(const Colour &, bool depthTest, bool wireframe = false) = 0;
    virtual bool        setUnlitMaterial(MaterialId, const Colour &) = 0;
    /// Selection silhouette: unlit colour drawn on BACK faces only, so a copy of the
    /// mesh scaled up slightly (~4%) renders as a clean outline band around the
    /// original (inverted hull). Depth-tested, so occluders still hide it.
    virtual MaterialId  createOutlineMaterial(const Colour &) = 0;
    /// A line list (pairs of points) or, with `strip`, a connected polyline.
    /// Attach with attachMesh like any mesh. One pixel wide.
    virtual MeshId      createLineMesh(const std::vector<Vec3> &points, bool strip) = 0;

    // ---- Particles: externally-simulated particles drawn as camera-facing quads.
    // The set rides on a node for ownership (removeNode frees it) but instance
    // positions are WORLD-space — the document simulates in world space.
    /// Creates (or replaces) the node's billboard set: up to `capacity` quads,
    /// textured by `texture` (0 = untextured white), additive (src-alpha, one) or
    /// alpha-blended. Depth test on, depth write off, drawn after opaques.
    virtual bool createBillboardSet(NodeId, TextureId texture, bool additiveBlend,
                                    unsigned capacity) = 0;
    /// Replaces the set's instances each frame; count above capacity is clamped.
    virtual bool setBillboards(NodeId, const BillboardInstance *, size_t count) = 0;
    /// Removes the node's billboard set (removeNode does this too). KEPT as the
    /// explicit counterpart of createBillboardSet: the document turns a particle
    /// system off without destroying its node, and tests/particles pins that.
    virtual bool destroyBillboardSet(NodeId) = 0;

    // ---- Lights (step 5): a node may carry one light. Directional and spot lights
    // shine down the node's -Y (the document's convention: identity = straight down).
    virtual bool        setLight(NodeId, const LightDesc &) = 0;   // creates or updates
    /// KEPT as the explicit counterpart of setLight: a node may stop being a light
    /// without being removed (the document changes a node's type in place).
    virtual bool        removeLight(NodeId) = 0;

    // ---- Global illumination (GI_SPEC.md). Scene-level, like fog and sky. ----
    /// Applies the GI state idempotently, rebuilding whatever changed. Passing the
    /// same params twice is cheap; GiMode::Off tears everything down. Modes the
    /// backend has not implemented yet degrade to Off (true is still returned so a
    /// document saved with a future mode keeps loading). Instant Radiosity is
    /// per-scene: its virtual point lights live in this scene only.
    virtual bool        setGlobalIllumination(const GiParams &) = 0;
    /// Re-runs the active GI solution against the scene's current state (the
    /// driving light moved, geometry changed). No-op when GI is off. IR re-traces
    /// in milliseconds at editor quality; callers may invoke this per edit.
    virtual void        refreshGlobalIllumination() = 0;
};

/// A view onto a Scene, rendering into a native window supplied by the host or
/// into an offscreen texture. Owned by the Engine: destroy with Engine::destroyView().
class View {
public:
    virtual ~View() = default;
    virtual const std::string &name() const = 0;
    /// Binds a Scene to this View. Call after createScene(); a View renders nothing
    /// until a Scene is attached. A View holds at most one Scene: binding a second
    /// while one is attached fails (false, lastError()). Pass null to detach.
    virtual bool setScene(Scene *) = 0;
    virtual Scene *scene() const = 0;
    /// Full camera state in one call (step 5). The document camera is pushed
    /// through this every frame. This is the ONLY way to move a View's camera.
    virtual void setCamera(const CameraDesc &) = 0;
    /// Clear colour behind the scene (the document's flat sky colour). Cheap to
    /// call with the same value; a change rebuilds the view's compositor workspace.
    virtual void setBackground(const Colour &) = 0;
    /// KEPT: cheap introspection the host needs to avoid redundant (workspace-
    /// rebuilding) setBackground calls, and pinned by the engine suites.
    virtual Colour background() const = 0;
    /// Shadow maps for this view (PSSM for directional, focused for point/spot;
    /// lights opt in with LightDesc::castShadows). Off by default; toggling rebuilds
    /// the view's workspace.
    virtual void setShadows(bool) = 0;
    virtual bool shadows() const = 0;
    /// A disabled View is skipped by renderOneFrame(). Hidden viewports MUST be
    /// disabled — the backend otherwise keeps drawing them at full cost.
    virtual void setEnabled(bool) = 0;
    /// KEPT: cheap introspection — hosts query it before pushing per-frame state
    /// into a View they may have disabled; pinned by the engine suites.
    virtual bool isEnabled() const = 0;
    virtual void resize(unsigned width, unsigned height) = 0;
    virtual unsigned width() const = 0;
    virtual unsigned height() const = 0;
    /// Hardware anti-aliasing (MSAA) for this view's render target: 1 = off,
    /// 2/4/8 typical (values are rounded down to a power of two and clamped).
    /// NOT cheap on change: the render target is recreated — on-screen at the
    /// next frame (the resize path), offscreen immediately. Calling again with
    /// the value already requested is free, so hosts may push it per frame.
    /// The driver may clamp the request (Vulkan only guarantees 1 and 4);
    /// sampleCount() reports the ACHIEVED count once the target exists.
    virtual void setSampleCount(unsigned samples) = 0;
    virtual unsigned sampleCount() const = 0;
    /// KEPT: cheap introspection — readPixels() only works offscreen, so callers
    /// (thumbnails, tests) branch on this; pinned by the engine suites.
    virtual bool isOffscreen() const = 0;
    /// Reads this View's rendered pixels back to the CPU. Offscreen Views only —
    /// returns false for on-screen windows. This is the thumbnail path, and what
    /// makes the engine testable without a window.
    virtual bool readPixels(Image &out) = 0;
};

/// Owns the device and every Scene and View.
///
/// ONE PER PROCESS. The backend is a process-wide singleton; create() refuses to
/// make a second Engine while one is alive (returns null + error). Destroying it
/// and creating another later is supported (tests/engine/test_engine_recreate) —
/// this needs the Ogre-Next patch recorded in OGRE_PLATFORM_DEPS.md.
///
/// LIFETIME CONTRACT: every View and Scene pointer handed out is owned by the
/// Engine and dies with it. Hosts that cache a View* (e.g. a widget) must call
/// destroyView() before the Engine is destroyed, or must check the Engine is still
/// alive before touching the pointer — see EngineViewWidget for the pattern.
class Engine {
public:
    virtual ~Engine() = default;

    /// Creates the engine. Returns null on failure and fills `error`.
    static std::unique_ptr<Engine> create(const EngineConfig &, std::string &error);
    /// True while an Engine exists in this process.
    static bool isAlive();

    /// ORDER MATTERS. A View must be created before any Scene: the underlying engine
    /// only starts its material and buffer systems when the first render target
    /// exists, and creating a Scene before that dereferences null.
    ///   createView(...)  ->  createScene(...)  ->  view->setScene(scene)
    /// Names must be unique among live Views; a duplicate returns null (lastError()).
    virtual View  *createView(const std::string &name,
                              NativeWindowHandle, unsigned width, unsigned height,
                              const Colour &background) = 0;
    /// An offscreen View: renders to a texture instead of a window. Needs no native
    /// handle, so it works headless. Used for thumbnails, asset previews and tests.
    virtual View  *createOffscreenView(const std::string &name,
                                       unsigned width, unsigned height,
                                       const Colour &background) = 0;
    /// Releases the View's window/texture and camera. Any Scene it showed survives.
    /// Null or unknown pointers are ignored.
    virtual void   destroyView(View *) = 0;

    /// Returns null if called before the first createView()/createOffscreenView(),
    /// or if the name is already in use (lastError()).
    virtual Scene *createScene(const std::string &name) = 0;
    /// Destroys the Scene and every node, mesh and material it owns. Views bound to
    /// it are detached first (they stay alive, showing nothing).
    virtual void   destroyScene(Scene *) = 0;

    /// Draws every enabled View once. The host owns the loop and calls this.
    virtual void renderOneFrame() = 0;

    /// Shadow filter quality for EVERY shadowed light in EVERY scene — the
    /// backend's PBR pipeline has one global filter, not a per-light one
    /// (Hard = PCF 2x2, Soft = PCF 4x4, VerySoft = PCF 6x6). Callers with
    /// per-light document settings push the strongest requested quality.
    /// Cheap: takes effect next frame, no material rebuild. Default: Soft.
    virtual void setShadowFilter(ShadowFilter) = 0;
    virtual ShadowFilter shadowFilter() const = 0;

    /// Shadow-map resolution for EVERY shadowed light in EVERY scene — global,
    /// like the filter: the backend renders all shadow maps into one fixed atlas
    /// (PSSM splits + two focused maps) whose sizes derive from this base value
    /// (split 0 and the focused maps at `pixels`, further splits at half).
    /// Callers with per-light document settings push the LARGEST requested size.
    /// NOT cheap: changing it tears down and rebuilds the shadow node and every
    /// workspace that references it — call on change only, never per frame.
    /// Clamped to [256, 8192]. Default: 2048.
    virtual void setShadowResolution(unsigned pixels) = 0;
    virtual unsigned shadowResolution() const = 0;

    /// Reason for the most recent failure; empty if none.
    virtual const std::string &lastError() const = 0;
};

}}  // namespace jahshaka::engine
