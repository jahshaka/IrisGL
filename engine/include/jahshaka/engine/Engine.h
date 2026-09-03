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
    /// Ambient light as a hemisphere pair: `upper` is what a surface facing +Y
    /// receives, `lower` what a surface facing -Y receives. Kept as the simple
    /// entry point (previews, thumbnails, tests); it is expressed EXACTLY in the
    /// spherical harmonics setAmbientSh() takes — see the note there about the
    /// scale of a flat (upper == lower) ambient.
    virtual void        setAmbient(const Colour &upper, const Colour &lower) = 0;
    /// Ambient light as 9 spherical-harmonic coefficients per channel, the form
    /// a real sky integrates to (a hemisphere pair can only ever say "up" and
    /// "down"). Layout: 9 groups of 3 floats (r, g, b), in the basis order
    ///     1, y, z, x, x*y, y*z, 3z^2 - 1, z*x, x^2 - y^2
    /// over WORLD axes, evaluated for the shading normal. The value the sum
    /// produces is a MEAN INCIDENT RADIANCE (irradiance / pi), the same unit the
    /// two hemisphere colours above are in, so a uniform white environment of
    /// radiance L is sh[0..2] = L and the rest zero.
    /// This is the only ambient path the backend has: setAmbient() converts.
    virtual void        setAmbientSh(const float sh[27]) = 0;
    /// Exponential distance fog (+ optional height layer) on lit (PBR) surfaces —
    /// see FogDesc for the model. Unlit overlays (gizmos, wires, billboards) and
    /// the sky are never fogged. Off by default, and OFF IS EXACT: a disabled
    /// FogDesc leaves the scene rendering the very same pixels it did before fog
    /// was ever mentioned. Cheap to call every frame while enabled; the enabled
    /// EDGE costs a shader rebuild (fog is a shader variant, not a uniform).
    virtual void        setFog(const FogDesc &) = 0;
    /// Textured sky behind everything: an equirectangular (lat-long) image.
    /// SkyMode::NoSky removes it (the View's background shows). Cubemap skies go
    /// through setSkyCubemap() — this call rejects SkyMode::Cubemap.
    virtual bool        setSky(SkyMode, TextureId) = 0;
    /// Cubemap sky from six face textures, in the order +X, -X, +Y, -Y, +Z, -Z,
    /// each face seen from INSIDE the cube looking down that WORLD axis (the
    /// backend converts to whatever handedness its cubemaps use). Also feeds
    /// environment reflections (IBL) from the same faces.
    virtual bool        setSkyCubemap(const TextureId faces[6]) = 0;
    /// Environment reflections (IBL) WITHOUT touching the sky: six square face
    /// textures (+X, -X, +Y, -Y, +Z, -Z, world axes, all the same size) become
    /// the reflection cubemap every PBR material samples. This is how
    /// equirectangular and CPU-baked skies (gradient, realistic) get the
    /// reflections cubemap skies already have — the host resamples its equirect
    /// image into six faces and pushes them here. The mip chain is a GGX
    /// (roughness) PREFILTER, not a box mip chain, so a rough metal reads the
    /// hemisphere around its reflection vector instead of one blurred face.
    /// Passing six zero ids clears the reflections. The face textures are
    /// copied; the caller may destroy them afterwards.
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

    // ---- Rigs: GPU skinning (GPU_SKINNING_SPEC) ----
    /// Like attachMesh, but the mesh deforms on the GPU: the host pushes bone
    /// poses (setBonePoses) instead of vertices, and the vertex shader skins
    /// position, normal AND tangent.
    ///
    /// SEPARATE entry point on purpose — the backend must know the mesh is
    /// skinned BEFORE the renderable exists, so attaching first and skinning
    /// later would silently produce an unskinned object.
    ///
    /// The mesh must have been created with MeshData::hasSkinData(); its blend
    /// indices name bones of `rig`. Several nodes may attach the same mesh and
    /// the same rig and still pose independently — one rig instance per node.
    /// Refuses (lastError()) a mesh with no skin data, a rig whose bones do not
    /// cover the mesh's blend indices, an empty rig, a rig whose parent indices
    /// are out of range or cyclic, or a rig with more than 256 bones (which is
    /// attached UNSKINNED at bind pose, with a warning, rather than crashing).
    /// Bone ORDER is free — the index is what the vertex data names.
    virtual bool        attachSkinnedMesh(NodeId, MeshId, MaterialId, const SkeletonDesc &) = 0;
    /// True when the node carries a GPU-skinned mesh with a live rig.
    virtual bool        hasSkeleton(NodeId) const = 0;
    /// The node's bone names, in rig index order. Empty when it has no rig.
    virtual std::vector<std::string> boneNames(NodeId) const = 0;
    /// The node's pose: `count` entries, index-parallel to the rig's bones, each
    /// LOCAL to its parent bone. This is the per-frame call — everything else on
    /// the rig surface is event-driven. False (lastError()) when the node has no
    /// rig or `count` does not match the rig's bone count.
    virtual bool        setBonePoses(NodeId, const BonePose *poses, size_t count) = 0;
    /// Reads back the bone matrices the vertex shader is actually handed for this
    /// node: `count` bones in rig order, each a ROW-MAJOR 3x4 (12 floats, so
    /// `out` holds count*12), WORLD-relative — the node's own transform is folded
    /// in, because a skinned vertex is never multiplied by a world matrix
    /// separately. Resolved as of the last rendered frame. False (lastError())
    /// when the node has no rig or `count` misses the rig's bone count.
    /// The read-back surface for the pose: what proves GPU and CPU skinning agree.
    virtual bool        boneMatrices(NodeId, float *out, size_t count) const = 0;

    // ---- Clips (ANIMATION_ENGINE_MIGRATION_SPEC) ----
    /// Attaches clips to a node that already carries a rig. IDEMPOTENT per clip
    /// id: attaching a clip whose id is already on the node is a no-op.
    ///
    /// EVERY clip a node will use must be attached BEFORE any of them is
    /// enabled. This is not a style preference: the engine's own
    /// addAnimationsFromSkeleton push_backs into the vector its list of ACTIVE
    /// animations holds raw pointers into, and it does not fix that list up —
    /// so attaching while something plays dangles every active clip. The call
    /// therefore REFUSES (lastError()) while any clip on the node is enabled.
    ///
    /// Attaching the first clip also takes the node OUT of manual-bone mode:
    /// a manual bone is not reset to the bind pose before a clip accumulates,
    /// so an enabled clip would ADD to whatever setBonePoses last wrote. Bones
    /// explicitly marked by setBoneManual keep their override.
    ///
    /// Refuses: a node with no rig; a track naming a bone the rig does not
    /// have; unsorted, duplicated or empty key times; a clip with no tracks.
    /// A clip whose length is <= 0 is PADDED to a minimum length and reported
    /// in the log, never refused.
    virtual bool attachClips(NodeId, const ClipDesc *clips, size_t count) = 0;
    /// The node's clip names, in attach order — including any uniquifying
    /// suffix the backend added for a collision. Empty when it has no rig.
    virtual std::vector<std::string> clipNames(NodeId) const = 0;

    /// THE per-frame clip call. Absolute times only. Clips the array does not
    /// name are disabled. Weights are raw intent; the backend normalizes them
    /// PER BONE and honours manual-bone overrides (a manual bone gets zero
    /// weight from every clip, so the override really overrides).
    ///
    /// NOTE, and it must be designed for rather than discovered: with NO clip
    /// enabled the engine does not reset to the bind pose at all — the pose
    /// FREEZES wherever it was. "Stop" means one clip enabled at t = 0, or a
    /// setBonePoses write, never an empty state array.
    virtual bool setClipStates(NodeId, const ClipState *states, size_t count) = 0;

    /// Per-bone override channel. A manual bone keeps whatever setBonePoses
    /// wrote and is excluded from every clip's weighting.
    virtual bool setBoneManual(NodeId, const std::string &bone, bool manual) = 0;

    /// Reads back the EVALUATED pose: `count` bones in rig order, each LOCAL to
    /// its parent bone (a root bone: local to the mesh node) — the same frame
    /// setBonePoses writes in. Resolved as of the last rendered frame.
    virtual bool bonePoses(NodeId, BonePose *out, size_t count) const = 0;

    /// The effective per-bone weight the backend applied for `clip`, in rig
    /// bone order (0 for a bone the clip does not animate). The test and
    /// diagnostic surface for the normalization rule; empty on any error.
    virtual std::vector<float> clipBoneWeights(NodeId, const std::string &clip) const = 0;

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

    // ---- Particles (PARTICLES_FX2_SPEC.md): the engine SIMULATES these ----
    // Unlike billboard sets, the host pushes PARAMETERS, not particles: emission,
    // forces, colour-over-life and spin run inside the engine (SIMD, on worker
    // threads) and advance on every renderOneFrame. The document owns the
    // authoring values and its own clock scalar; it never integrates anything.
    //
    // One node = one particle-system DEFINITION. The definition carries the quota,
    // the material and the visibility flag, so two emitters in a scene are fully
    // independent. Definitions cannot be individually destroyed by the backend
    // (there is no such API), so the engine keeps each node's topology FROZEN —
    // one emitter of the chosen shape plus a fixed, defaults-neutral affector set —
    // and recycles abandoned definitions through a per-scene pool.

    /// Creates or updates the node's particle system. Scalar changes (rate,
    /// velocity, colour keys, forces...) are applied in place. A TOPOLOGY change —
    /// emitter shape or count, affector kinds, quota bucket, orientation, blend
    /// mode or texture — rebuilds the definition and recycles the old one.
    /// The system rides on the node: it moves with it, hides with it, and
    /// removeNode frees it. False with lastError() set; never throws.
    virtual bool setParticleSystem(NodeId, const ParticleSystemDesc &) = 0;
    /// KEPT as the explicit counterpart of setParticleSystem: a node may stop
    /// being an emitter without being removed. Live particles vanish with it.
    virtual bool removeParticleSystem(NodeId) = 0;
    /// How many particles are currently alive in the node's system, for tests and
    /// the properties panel. 0 when the node has no system. SIMD-rounded up.
    virtual unsigned particleCount(NodeId) const = 0;
    /// DIAGNOSTIC: how many particle definitions this Scene has ever created.
    /// Definitions cannot be destroyed before the Scene is, so this number never
    /// falls — it is the leak the recycling pool exists to bound, and the
    /// particle suites assert on it. Never call it from UI code.
    virtual unsigned particleDefinitionsCreated() const = 0;

    // ---- Lights (step 5): a node may carry one light. Directional and spot lights
    // shine down the node's -Y (the document's convention: identity = straight down).
    virtual bool        setLight(NodeId, const LightDesc &) = 0;   // creates or updates
    /// KEPT as the explicit counterpart of setLight: a node may stop being a light
    /// without being removed (the document changes a node's type in place).
    virtual bool        removeLight(NodeId) = 0;

    // ---- Decals (DECALS_SPEC.md): a node may carry one projected-texture decal.
    // A decal is an oriented box that overwrites base colour / roughness /
    // metalness on the surfaces inside it, projecting down the node's -Y (the
    // same convention as lights). It draws nothing itself: the PBR shader
    // consumes it through the Forward+ clustered list.
    /// Creates or updates the node's decal. False (lastError()) when the desc
    /// carries no diffuse texture, or one that did not come from
    /// loadDecalTexture().
    virtual bool        setDecal(NodeId, const DecalDesc &) = 0;
    /// KEPT as the explicit counterpart of setDecal: a node may stop being a
    /// decal without being removed (the document changes a node's type, or the
    /// user clears the image).
    virtual bool        removeDecal(NodeId) = 0;
    /// Loads an image into the DEDICATED, fixed-geometry decal atlas for `kind`
    /// and returns a texture id usable in DecalDesc. Images are resampled into
    /// the atlas geometry (aspect preserved, padded with transparent pixels —
    /// alpha is the decal mask, so padding is invisible).
    ///
    /// NOT interchangeable with loadTexture(): decals sample one Type2DArray
    /// per channel and carry a slice index into it, so every decal image must
    /// share one resolution/format/mip-count pool. loadTexture() puts images in
    /// pool 0 alongside ordinary PBR maps (wrong slices) and its grayscale
    /// branch produces a non-batched texture the backend refuses outright.
    ///
    /// Returns 0 with a clear lastError() when the atlas is FULL — never a
    /// silent fallback: an overflowing decal would sample another decal's image
    /// with no warning at all.
    virtual TextureId   loadDecalTexture(const std::string &path, DecalMap kind) = 0;
    /// How many slices the `kind` atlas has, and how many are already taken.
    /// The UI surfaces "decal image budget full" from this rather than guessing.
    virtual unsigned    decalAtlasCapacity(DecalMap kind) const = 0;
    virtual unsigned    decalAtlasUsed(DecalMap kind) const = 0;

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

    // ---- Planar reflections (PLANAR_REFLECTIONS_SPEC.md). Scene-level, like GI. ----
    /// Applies the reflection state idempotently. Pushing the same params twice is
    /// free; a CHANGE rebuilds the whole arm (render targets, cameras, private
    /// workspaces) and a budget change additionally recompiles PBS shaders, so
    /// hosts may call this every frame but must not animate the values.
    /// `budget == 0` tears everything down and costs nothing.
    ///
    /// Only ONE scene per process can have reflections at a time: the receiving
    /// half lives on the process-wide HlmsPbs, exactly like VCT/PCC. The last
    /// scene to enable owns the binding; enabling on a second scene disables the
    /// first (which is why the host arms this on the editor scene only).
    virtual bool        setPlanarReflections(const PlanarReflectionParams &) = 0;
    /// Makes (or un-makes) a node a reflection plane. The node must already have
    /// a mesh attached, and that mesh must be PLATE-LIKE — its thinnest local
    /// extent no more than a tenth of the next — because the plane, its size and
    /// its normal are all derived from the mesh's own bounds. A sphere or a cube
    /// is refused (false, lastError()); the 20-degree matching rule would make it
    /// look broken rather than merely wrong.
    ///
    /// The plane's normal is the node's thin axis in the POSITIVE direction: the
    /// top of a floor reflects, the underside does not. The reflector is excluded
    /// from its own reflection render, so a mirror never contains itself.
    /// Reflectors survive `setPlanarReflections` changes; the flag is remembered
    /// even while the budget is 0.
    virtual bool        setNodePlanarReflector(NodeId, bool) = 0;
    virtual bool        nodePlanarReflector(NodeId) const = 0;
    /// How many reflection planes actually rendered last frame — the "achieved"
    /// number against the requested budget (planes off screen do not render).
    /// 0 when reflections are off or nothing has rendered yet.
    virtual int         activePlanarReflectors() const = 0;
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

    /// The post-processing chain for this View (POST_CHAIN_SPEC.md): HDR +
    /// filmic tonemap, bloom, SSAO, SMAA, SSR, refractive glass.
    ///
    /// IGNORED ON OFFSCREEN VIEWS, always and by construction — postFx() then
    /// reports what was asked for, and the chain stays the simple one. That is
    /// what keeps thumbnails, material previews and every pixel suite exact.
    ///
    /// Cheap to call with an unchanged value (hosts may push per frame). A
    /// change to an ENABLE flag rebuilds the workspace; a change to a tuning
    /// value is a uniform and rebuilds nothing.
    virtual void setPostFx(const PostFxDesc &) = 0;
    virtual const PostFxDesc &postFx() const = 0;
    /// How many times this View has (re)built its compositor workspace — the
    /// structurally expensive operation behind setShadows(), setBackground(),
    /// resize(), setSampleCount() and the engine's shadow-atlas rebuild. Starts
    /// at 0 and reaches 1 when a Scene is first bound. Hosts use it to verify
    /// that a per-frame push really was free; tests use it to pin the fact that
    /// there is exactly ONE place a workspace is created (POST_CHAIN_SPEC.md).
    virtual unsigned workspaceGeneration() const = 0;
    /// How many frames this View has actually drawn AND presented since its
    /// current Scene was bound — the honest "are there real pixels in that
    /// window yet?" signal. A frame counts only when the View was enabled, had
    /// a live workspace and a bound Scene while Engine::renderOneFrame ran, so
    /// a hidden, scene-less or workspace-less View never inflates it. Binding a
    /// Scene (or detaching one) resets it to 0; a workspace REBUILD does not —
    /// the pixels of the previous frame are still on screen.
    ///
    /// Hosts use it to know when the window stopped showing stale pixels: the
    /// editor's loading cover (src/viewport/viewportcover.h) is on screen until
    /// this passes its threshold.
    virtual unsigned long long framesPresented() const = 0;
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

    // ---- Simulation clock (PARTICLES_FX2_SPEC.md) ----
    // The engine advances its own particle simulation inside renderOneFrame,
    // from the backend's frame-time source. These two verbs are the ONLY control
    // the host has over it, and both are PROCESS-WIDE, not per scene and not per
    // view — the backend has exactly one frame-time source. "Freeze the editor's
    // particles while the player window runs" is therefore not expressible; the
    // document owns one clock scalar and pushes it here, the way the animation
    // migration does.
    /// Multiplies the frame delta every simulation reads. 1 = wall clock,
    /// 0 = frozen, 2 = double speed. Also cancels any fixed frame delta.
    virtual void setParticleTimeScale(float scale) = 0;
    virtual float particleTimeScale() const = 0;
    /// Replaces the wall clock with a FIXED step, in seconds — the same delta
    /// every frame regardless of how long the frame took. Deterministic enough
    /// for pixel gates and thumbnail warm-ups (emission is still randomised, so
    /// those stay statistical); 0 restores the wall clock at scale 1.
    virtual void setFixedFrameDelta(float seconds) = 0;
    virtual float fixedFrameDelta() const = 0;

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

    /// Shadow-caster geometry optimization — see EngineConfig::optimizeShadowMeshes.
    /// PROCESS-WIDE and consumed when a mesh is BUILT: changing it re-decides the
    /// question for meshes created afterwards and leaves existing ones alone.
    /// That is why it is an application preference, not a per-scene setting.
    virtual void setShadowMeshOptimization(bool on) = 0;
    virtual bool shadowMeshOptimization() const = 0;

    // ---- Persistent shader cache (SHADER_CACHE_SPEC.md) ----
    // Three layers behind one fingerprinted container: the Vulkan pipeline
    // cache (driver ISA), the microcode cache (SPIR-V), and the Hlms disk cache
    // (preprocessed shader source). All of it is DERIVED DATA: on any doubt the
    // backend deletes the directory and starts cold rather than feed a
    // half-written blob to a driver.
    //
    // The cache is loaded once, inside the first createView() — nothing here
    // needs calling to make it work. These verbs exist so the application can
    // SHOW what it did and let a user throw it away.

    /// What is on disk and what happened this run. Cheap enough to call from a
    /// settings page; it stats a handful of files.
    virtual ShaderCacheStats shaderCacheStats() const = 0;
    /// Writes the cache now, if anything new has been compiled since the last
    /// write. Called on clean shutdown and once a compile burst has settled;
    /// safe (and a no-op) when the cache is disabled or nothing is dirty.
    /// False means the write failed — the previous cache, if any, is untouched.
    virtual bool saveShaderCache() = 0;
    /// Deletes every cached file. The next launch is cold. Always safe: the
    /// running process keeps its in-memory shaders.
    virtual bool clearShaderCache() = 0;
    /// The startup progress counter's source: shaders compiled so far, shaders
    /// served from the cache so far, and how many the last saved run needed in
    /// total (0 = never saved, so no denominator exists yet). Two atomic reads;
    /// no disk, safe to poll on a timer.
    virtual void shaderBuildProgress(unsigned &compiled, unsigned &fromCache,
                                     unsigned &expected) const = 0;

    /// Reason for the most recent failure; empty if none.
    virtual const std::string &lastError() const = 0;
};

}}  // namespace jahshaka::engine
