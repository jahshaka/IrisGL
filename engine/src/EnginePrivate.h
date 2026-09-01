#pragma once
// Internal header of the Ogre-Next 4.0 backend.
//
// THE ONE-DIRECTORY RULE: engine/src/ is the only directory that includes Ogre.
// This header is private to it — it is never installed and must never be included
// from outside irisgl/engine/src. If Ogre appears anywhere else, the boundary has
// been breached.
//
// Verified behaviours this backend depends on (spikes/qt-ogre-next/FINDINGS.md and
// spikes/headless-vulkan/README.md):
//   * Vulkan supports MULTIPLE on-screen windows; GL3Plus does not (single mGlobalVao).
//   * v1 meshes render NOTHING on Vulkan — geometry is built as v2 buffers directly.
//   * Hlms shader templates are required at runtime, not optional sample data.
//   * A render target must exist BEFORE Hlms registration or any SceneManager;
//     for a purely offscreen engine a surfaceless "null" window satisfies this.
//   * Teardown order is load-bearing: workspaces -> scenes -> our MeshPtrs ->
//     MeshManager::removeAll -> Root. A MeshPtr outliving Root hits a dead VaoManager.
//   * No Ogre exception may escape: every virtual is wrapped and translated.
#include "jahshaka/engine/Engine.h"

#include <OgreRoot.h>
#include <OgreAbiUtils.h>
#include <OgreWindow.h>
#include <OgreCamera.h>
#include <OgreSceneManager.h>
#include <OgreItem.h>
#include <OgreMesh2.h>
#include <OgreMeshManager2.h>
#include <OgreSubMesh2.h>
#include <OgreArchiveManager.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsListener.h>
#include <OgreHlmsPbs.h>
#include <OgreHlmsUnlit.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreTextureGpuManager.h>
#include <OgreTextureFilters.h>
#include <OgreStagingTexture.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreImage2.h>
#include <OgreTextureGpu.h>
#include <OgreAsyncTextureTicket.h>
#include <OgreLogManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreHlmsSamplerblock.h>
#include <set>
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <Compositor/OgreCompositorShadowNodeDef.h>
#include <OgreRenderSystemCapabilities.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreVertexArrayObject.h>
#include <ParticleSystem/OgreBillboardSet2.h>
#include <ParticleSystem/OgreParticleSystemManager2.h>
#include <OgreForwardPlusBase.h>
#include <InstantRadiosity/OgreInstantRadiosity.h>
#include <Vct/OgreVctVoxelizer.h>
#include <Vct/OgreVctLighting.h>
#include <Cubemaps/OgreParallaxCorrectedCubemapAuto.h>
#include <Cubemaps/OgrePccPerPixelGridPlacement.h>

// X11 is the only on-screen window path today (Ogre Vulkan/XCB). Other
// platforms build headless-only until they grow a native window backend
// (macOS: CAMetalLayer + VK_EXT_metal_surface — see DOCS/HANDOFF.md §7).
#ifdef __linux__
#    include <X11/Xlib.h>
#endif
#include <atomic>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jahshaka { namespace engine {
// The backend's own namespace: these types and helpers are shared between the
// TUs under engine/src and by nothing else (they used to live in one anonymous
// namespace, when the backend was a single translation unit).
namespace detail {

inline Ogre::Vector3     toOgre(const Vec3 &v)   { return Ogre::Vector3(v.x, v.y, v.z); }
inline Ogre::ColourValue toOgre(const Colour &c) { return Ogre::ColourValue(c.r, c.g, c.b, c.a); }

/// Names handed to Ogre must be unique for the life of the process (a destroyed
/// scene may be recreated under the same name while stale resources linger).
inline std::string processUniqueName(const char *prefix) {
    static std::atomic<unsigned> counter{0};
    return std::string(prefix) + "_" + std::to_string(++counter);
}

// Every backend virtual is wrapped: `JAH_TRY { ... } JAH_CATCH(errSink, failValue)`.
// Ogre throws Ogre::Exception; its own allocations may throw std::bad_alloc.
#define JAH_TRY try
#define JAH_CATCH(sink, ret)                                                          \
    catch (Ogre::Exception &e) { (sink) = e.getFullDescription(); return ret; }       \
    catch (std::exception &e)  { (sink) = std::string("engine: ") + e.what(); return ret; }

class OgreEngine;

// ---------------------------------------------------------------------------
// Visibility-flag bits (user bits; Ogre reserves the top two for layer state).
// Every object keeps kVisibleBit so default cameras/compositor masks (all ones)
// draw it. kGiGeometryBit marks items whose surfaces bounce light for GI: PBR
// items only — never the sky, unlit overlays, line meshes or billboards, which
// would otherwise occlude rays or raycast as garbage triangles (a non-indexed
// line VAO reads as a vertex triangle list). kGiLightBit marks exactly the one
// light Instant Radiosity traces from (InstantRadiosity::mLightMask).
constexpr Ogre::uint32 kVisibleBit     = 1u;
constexpr Ogre::uint32 kGiGeometryBit  = 1u << 1;
constexpr Ogre::uint32 kGiLightBit     = 1u << 2;

// ---------------------------------------------------------------------------
// Linear distance fog (media/Hlms/Jahshaka/JahFog_piece_ps.any, attached to every
// lit PBS datablock). Parameters are per-scene, keyed by SceneManager: the listener
// is global to HlmsPbs, but preparePassBuffer receives the SceneManager of the pass
// being built. The two float4s are ALWAYS appended — fog off writes enabled=0 — so
// the pass-buffer layout is constant and toggling fog is a uniform change, never a
// shader recompile or Hlms cache event.
struct FogParams {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    float start = 0.0f, end = 1.0f;
    bool  enabled = false;
};

class FogHlmsListener final : public Ogre::HlmsListener {
public:
    Ogre::uint32 getPassBufferSize(const Ogre::CompositorShadowNode *, bool /*casterPass*/,
                                   bool, Ogre::SceneManager *) const override;
    float *preparePassBuffer(const Ogre::CompositorShadowNode *, bool, bool,
                             Ogre::SceneManager *sceneManager, float *passBufferPtr) override;

    /// The per-scene fog table. OgreScene::setFog registers, the scene teardown
    /// unregisters, preparePassBuffer looks up.
    static void      registerScene(const Ogre::SceneManager *sm, const FogParams &p);
    static void      unregisterScene(const Ogre::SceneManager *sm);
    static FogParams lookup(const Ogre::SceneManager *sm);

private:
    static std::map<const Ogre::SceneManager *, FogParams> sFogParams;   // render thread only
};
extern FogHlmsListener gFogListener;

// ---------------------------------------------------------------------------
class OgreScene final : public Scene {
public:
    OgreScene(Ogre::Root *root, Ogre::SceneManager *sm, const std::string &name,
              std::string &errorSink);
    ~OgreScene() override;

    const std::string &name() const override;

    void setAmbient(const Colour &upper, const Colour &lower) override;

    void setFog(bool enabled, const Colour &colour, float start, float end) override;

    /// Jahshaka's own sky: a UV sphere around the camera with an unlit textured
    /// material, depth test/write off, drawn first (render queue 0) so the scene
    /// paints over it. Ogre's SceneManager::setSky is not used — on Vulkan its
    /// Sky.material throws on 'sliceIdx' after the sky renderable is already
    /// attached, which then crashes in the render queue with a null datablock.
    bool setSky(SkyMode mode, TextureId texId) override;
    bool setSkyCubemap(const TextureId faces[6]) override;
    /// Environment reflections divorced from the sky geometry: the host pushes six
    /// resampled faces of its equirect/baked sky image. Six zero ids clear.
    bool setSkyReflection(const TextureId faces[6]) override;
    /// Builds (replacing any previous) the mipped reflection cubemap from six equal
    /// faces (+X,-X,+Y,-Y,+Z,-Z) and binds it on every PBR datablock.
    void buildReflectionCubemap(Ogre::TextureGpu *const tex[6]);
    /// Unbinds and destroys the reflection cubemap (no-op when there is none).
    void destroyReflection();
    /// Binds (or clears, when mReflectionTex is null) the scene's sky reflection
    /// cubemap on every PBR material's datablock. (Body in complete-class context,
    /// so it may call the private impl declared further down.)
    void applyReflectionToAll();
    /// Called by the engine before rendering: keep the sky centred on the camera and
    /// inside its far plane.
    void followCamera(const Ogre::Vector3 &camPos, float farClip);
    struct SkyFace { Ogre::Item *item; Ogre::MeshPtr mesh; std::string meshName, dbName; };
    std::vector<SkyFace> mSkyFaces;
    Ogre::TextureGpu *mReflectionTex = nullptr;   // sky cubemap on PBSM_REFLECTION
    void destroySky();
    bool removeNode(NodeId id) override;

    // ---- Hierarchy and transforms ----
    NodeId createNode(NodeId parent) override;
    bool setNodeParent(NodeId id, NodeId parent) override;
    void setNodeTransform(NodeId id, const Vec3 &pos, const Quat &rot, const Vec3 &scale) override;
    void setNodeVisible(NodeId id, bool visible) override;

    // ---- Meshes and materials ----
    MeshId createMesh(const MeshData &data) override;
    bool updateMeshVertices(MeshId id, const std::vector<float> &positions,
                            const std::vector<float> &normals) override;
    bool destroyMesh(MeshId id) override;
    MaterialId createPbrMaterial(const PbrParams &p) override;
    bool setPbrMaterial(MaterialId id, const PbrParams &p) override;
    bool destroyMaterial(MaterialId id) override;
    bool attachMesh(NodeId id, MeshId meshId, MaterialId matId) override;
    bool detachMesh(NodeId id) override;

    // ---- Textures ----
    TextureId loadTexture(const std::string &path, bool srgb) override;
    TextureId createTexture(unsigned w, unsigned h, const unsigned char *rgba, bool srgb) override;
    bool destroyTexture(TextureId id) override;
    bool setPbrTexture(MaterialId mat, PbrTextureSlot slot, TextureId texId) override;

    // ---- Overlay primitives ----
    MaterialId createUnlitMaterial(const Colour &c, bool depthTest, bool wireframe) override;
    MaterialId createOutlineMaterial(const Colour &c) override;
    bool setUnlitMaterial(MaterialId id, const Colour &c) override;
    MeshId createLineMesh(const std::vector<Vec3> &points, bool strip) override;

    // ---- Particles (billboard sets) ----
    // Ogre-Next's BillboardSet2 (ParticleFX2 core, lives in OgreNextMain — no
    // plugin needed): geometry is generated in the vertex shader from a read-only
    // buffer the ParticleSystemManager2 uploads each frame. The manager attaches
    // every set to the STATIC root scene node, so positions are world-space —
    // exactly how the document simulates. Requires
    // Hlms::_setHasParticleFX2Plugin(true) before shaders are built (ensureHlms).
    bool createBillboardSet(NodeId id, TextureId texId, bool additiveBlend,
                            unsigned capacity) override;
    bool setBillboards(NodeId id, const BillboardInstance *data, size_t count) override;
    bool destroyBillboardSet(NodeId id) override;

    // ---- Lights ----
    bool setLight(NodeId id, const LightDesc &d) override;
    bool removeLight(NodeId id) override;

    // ---- Global illumination (GI_SPEC.md phases 1-3) ----
    // Instant Radiosity traces rays from ONE chosen light against the scene's
    // PBR items and plants virtual point lights (LT_VPL) where the rays bounce.
    // The VPLs live in THIS SceneManager and ride its Forward+ clustered list —
    // nothing binds to the process-wide HlmsPbs, so IR never leaks into the
    // player/asset/preview scenes. (The sample's optional IrradianceVolume WOULD
    // be such a global binding — deliberately not used.)
    // VCT voxelizes the scene's PBR items over the GI bounds and cone-traces the
    // result; the hybrid adds a parallax-corrected cubemap probe grid whose
    // reflections blend with VCT's by distance (HlmsPbs PccVctMinDistance).
    // CAVEAT (GI_SPEC.md): setVctLighting/setParallaxCorrectedCubemap bind to the
    // process-wide HlmsPbs singleton — VCT GI is effectively editor-scene-only
    // in v1; the last scene to enable a VCT mode owns the binding, and other
    // scenes' geometry outside the voxel volume samples nothing (cones exit the
    // volume and add no light), so previews/thumbnails stay sane in practice.
    bool setGlobalIllumination(const GiParams &p) override;
    void refreshGlobalIllumination() override;

    Ogre::SceneManager *sceneManager() const;

    /// Releases everything in dependency order. Safe to call twice. Called by
    /// Engine::destroyScene and by the Engine destructor BEFORE Root dies.
    void destroy();

private:
    /// What a node owns. mNodes used to track only the SceneNode, leaking the
    /// Item, Light, mesh and datablock on removal (audit).
    struct Node {
        Ogre::SceneNode *node  = nullptr;
        Ogre::Item      *item  = nullptr;
        Ogre::Light     *light = nullptr;
        Ogre::SceneNode *lightNode = nullptr;   // internal child: -Y (document) -> -Z (Ogre)
        // Vestigial since the selftest-era addTestCube was pruned (nothing assigns
        // these any more); releaseNode still clears them so a future node-owned
        // mesh/datablock keeps the "dropped before Root" teardown guarantee.
        Ogre::MeshPtr    mesh;              // uniquely owned; MUST be dropped before Root
        std::string      meshName;
        std::string      datablockName;     // uniquely owned
        MeshId           meshRef     = 0;   // shared, owned by mMeshes
        MaterialId       materialRef = 0;   // shared, owned by mMaterials
        // Billboard set (particles): uniquely owned; freed by releaseBillboards
        // BEFORE the scene manager dies (its _destroy needs the live VaoManager).
        Ogre::BillboardSet       *billboards = nullptr;
        std::vector<Ogre::uint32> billboardHandles;   // live handles, dense, in order
        std::string               billboardDatablockName;
        unsigned                  billboardCapacity = 0;
    };
    struct MeshRec {
        Ogre::MeshPtr mesh; std::string name;
        // CPU-skinning support (MeshData::dynamic): the interleaved vertex array
        // (12 floats: pos3 normal3 tangent4 uv2) is kept so updateMeshVertices can
        // rewrite positions/normals while preserving tangents and uvs.
        bool dynamic = false;
        std::vector<float> interleaved;
    };
    struct MaterialRec { std::string datablockName; bool unlit = false; bool onTop = false; };
    struct TextureRec { Ogre::TextureGpu *texture = nullptr; std::string path; };

    void applyReflectionToAllImpl();

    Ogre::Hlms *hlmsFor(const MaterialRec &m) const;
    /// Removes the renderable from a node that references a SHARED mesh/material.
    void detachItem(Node &n);
    // Ogre::HlmsPbsDatablock::None is unspellable here: X11's `None` macro (this
    // file includes Xlib.h for window handles) eats the identifier.
    static constexpr auto kTransparencyNone = static_cast<Ogre::HlmsPbsDatablock::TransparencyModes>(0);
    static void applyPbr(Ogre::HlmsPbsDatablock *db, const PbrParams &p);
    /// Uploads MeshData as a v2 mesh: interleaved position/normal/tangent/uv, 16- or
    /// 32-bit indices. v1 meshes silently render nothing on Vulkan, so only this path
    /// exists. Every mesh carries tangents: HlmsPbs refuses to render a normal-mapped
    /// datablock on a mesh without them (throws, object falls back to flat grey).
    Ogre::MeshPtr buildMeshV2(const std::string &name, const MeshData &data,
                              std::vector<float> *interleavedOut = nullptr);
    /// Inward-facing UV sphere (radius 1); u = longitude, v = latitude — equirect mapping.
    Ogre::MeshPtr buildSkySphere(const std::string &name);
    /// Frees a node's billboard set and its datablock, in that order (the set
    /// references the datablock until it is destroyed). Safe to call twice.
    void releaseBillboards(Node &n);
    void releaseNode(Node &n);

    // ---- GI internals ----
    /// Resolves the driving light — the requested node's light, else the first
    /// directional, else any light — and marks exactly that light with
    /// kGiLightBit so InstantRadiosity's light mask selects it alone. Returns
    /// null when the scene has no light at all. (IR's own LT_VPL lights are not
    /// in mNodes and are skipped by IR itself.)
    Ogre::Light *markGiLight(NodeId requested);
    /// The GI working volume: the document's explicit bounds, or (min == max)
    /// the world AABB of every GI-participating item plus a margin.
    bool computeGiBounds(Ogre::Vector3 &mn, Ogre::Vector3 &mx) const;
    /// Re-traces Instant Radiosity against the scene as it is right now. Cheap
    /// enough (a few ms at editor quality) to run on every light move.
    /// Ogre::InstantRadiosity caches mesh data by raw VertexArrayObject* and
    /// downloaded images by TextureGpu* (OgreInstantRadiosity.h:238-244). Destroying
    /// a mesh/texture while IR is live leaves those caches dangling — the owner's
    /// scene-switch crash ("double free or corruption" tearing down a scene with IR
    /// enabled while the next project's assets churned). Ogre::VctVoxelizer has the
    /// same shape twice over: addItem keeps raw Item* until removeAllItems, and
    /// VctMaterial caches conversions by raw datablock pointer across builds.
    /// So every geometry/material/texture destroy path calls this BEFORE the
    /// object actually dies — IR's caches are freed EAGERLY here, because
    /// InstantRadiosity::freeMemory dereferences its VertexArrayObject* cache
    /// keys and calling it after the mesh died is itself the heap corruption.
    /// The rebuild still happens ONCE at frame time (bursty destroys = one
    /// rebuild); for VCT the flush tears the whole arm down and re-voxelizes
    /// from the LIVE scene, so a recycled pointer can never alias.
    void invalidateGiCaches();
public:
    /// Called by Engine::renderOneFrame before rendering.
    void applyPendingGi();
    /// Called by OgreView each frame with its camera position: the PCC probe
    /// blend tracks the viewer. No-op unless the hybrid mode is live.
    void updateGiTracking(const Ogre::Vector3 &camPos);
private:
    void rebuildGi();
    /// Voxelizes the scene's PBR items over computeGiBounds at quality-mapped
    /// resolution, (re)builds VctLighting and binds it to HlmsPbs. The voxelizer
    /// and lighting are recreated from scratch every time (see invalidateGiCaches).
    /// In hybrid mode also (re)builds the PCC probe grid.
    void rebuildVct();
    /// Builds the ParallaxCorrectedCubemapAuto probe grid over the same bounds
    /// and binds it with distance-blended VCT specular (PccVctMinDistance).
    void buildPcc(const Ogre::Aabb &aabb);
    /// Unbinds from HlmsPbs (when this scene owns the binding) and deletes the
    /// PCC, VctLighting and VctVoxelizer, in that order. Safe to call twice;
    /// must run BEFORE the SceneManager dies.
    void teardownVct();
    /// Deletes the radiosity solution and its VPL lights. Safe to call twice;
    /// must run BEFORE the SceneManager dies (the dtor destroys its lights).
    void teardownIr();
    /// Deletes every GI object (IR + VCT arms). Safe to call twice; must run
    /// BEFORE the SceneManager dies (VPL lights, probe workspaces, GI camera).
    void teardownGi();

    Ogre::SceneNode *node(NodeId id) const;
    /// Ids are monotonic per scene and never reused.
    NodeId track(const Node &n);


    Ogre::Root         *mRoot;
    Ogre::SceneManager *mSceneMgr;
    std::string         mName;
    std::string        &mError;
    std::map<NodeId, Node> mNodes;
    std::map<MeshId, MeshRec> mMeshes;
    std::map<MaterialId, MaterialRec> mMaterials;
    std::map<TextureId, TextureRec> mTextures;
    std::set<std::string> mTextureDirs;
    Ogre::SceneNode *mSkyNode = nullptr;
    Ogre::Item      *mSkyItem = nullptr;
    Ogre::MeshPtr    mSkyMesh;
    std::string      mSkyMeshName, mSkyDatablockName;
    Ogre::InstantRadiosity *mInstantRadiosity = nullptr;   // owned; null unless IR mode
    // VCT arm (null unless a VCT mode is live). Teardown order within the arm:
    // unbind HlmsPbs -> PCC -> VctLighting -> VctVoxelizer, all before the
    // SceneManager (probe workspaces and the GI camera live in it).
    Ogre::VctVoxelizer               *mVctVoxelizer = nullptr;
    Ogre::VctLighting                *mVctLighting  = nullptr;
    Ogre::ParallaxCorrectedCubemapAuto *mPcc        = nullptr;
    Ogre::Camera                     *mGiCamera     = nullptr;   // PCC build + tracking
    bool mGiCachesDirty = false;   // mesh/texture/material died while GI live; flush at frame time
    GiParams         mGi;                                  // last applied GI state
    TextureId           mNextTextureId = 0;
    NodeId              mNextId = 0;
    MeshId              mNextMeshId = 0;
    MaterialId          mNextMaterialId = 0;
};

// ---------------------------------------------------------------------------
class OgreView final : public View {
public:
    /// On-screen: `window` is set. Offscreen: `texture` is set. Never both.
    OgreView(Ogre::Root *root, Ogre::Window *window, Ogre::TextureGpu *texture,
             const std::string &name, unsigned w, unsigned h, const Colour &background,
             std::string &errorSink);
    ~OgreView() override;

    const std::string &name() const override;
    Scene *scene() const override;

    bool setScene(Scene *scene) override;

    /// Unbinds the scene: workspace and camera go, the scene itself survives.
    void detachScene();

    void setCamera(const CameraDesc &c) override;
    void setEnabled(bool on) override;
    Colour background() const override;
    bool shadows() const override;
    void setShadows(bool on) override;
    void setBackground(const Colour &c) override;
    /// The clear colour and the shadow node live in the workspace definition:
    /// rebuild definition + workspace, keeping scene, camera and enabled state.
    void rebuildWorkspaceDef();
    static constexpr const char *kShadowNodeName = "JahshakaShadowNode";
    /// Shadow-atlas rebuild support (Engine::setShadowResolution): the shadow node
    /// DEFINITION cannot be replaced while any workspace instantiates it, so the
    /// engine first drops every shadowed view's workspace (true = dropped, caller
    /// re-adds), then swaps the definition, then calls the restore below.
    bool dropWorkspaceForShadowRebuild();
    void recreateWorkspaceAfterShadowRebuild();
    bool isEnabled() const override;
    unsigned width()  const override;
    unsigned height() const override;
    bool isOffscreen() const override;

    /// Set by the engine for on-screen views ON X11: creates a fresh Ogre window on the
    /// same native handle at the given size and MSAA sample count (the "FSAA" misc param —
    /// BOTH window-creation sites must pass it or MSAA silently resets on resize).
    /// EMPTY on macOS: VulkanMetalWindow resizes its own surface (see applyPendingResize).
    std::function<Ogre::Window *(unsigned, unsigned, unsigned)> mCreateWindow;
    unsigned mPendingW = 0, mPendingH = 0;
    /// Runtime MSAA change is structurally a resize (Vulkan has no runtime
    /// setFsaa): non-zero = recreate the target at mRequestedSamples next
    /// applyPendingResize, even at the same size.
    unsigned mPendingSamples = 0;
    /// What the host asked for (sanitized). The engine initialises it for
    /// on-screen views from EngineConfig::sampleCount; offscreen views start
    /// at 1 (pixel-asserted readbacks stay exact unless a test opts in).
    unsigned mRequestedSamples = 1;

    void setSampleCount(unsigned samples) override;
    unsigned sampleCount() const override;

    /// On-screen resize is applied at frame time (applyPendingResize): by then Qt has
    /// resized the native window, and doing it once per frame coalesces layout bursts.
    void resize(unsigned w, unsigned h) override;

    bool readPixels(Image &out) override;

    /// The XCB window does not reallocate its depth buffer when the host resizes it
    /// (the swapchain follows the surface, the depth texture keeps its old size, and
    /// the mismatched framebuffer faults the GPU — Vulkan validation VUID 04533/04534),
    /// so on X11 a size change recreates the render window on the same native handle
    /// (mCreateWindow). Windows that implement requestResolution properly — macOS's
    /// VulkanMetalWindow does: destroySwapchain (which transitions colour AND depth to
    /// OnStorage) → setFinalResolution → createSwapchain — are resized in place instead.
    void applyPendingResize();
    /// Keeps the scene's sky sphere centred on this view's camera.
    void updateSky();
    /// Feeds the camera position to the scene's particle manager: billboard uploads
    /// are depth-sorted against it (matters for alpha-blended sets).
    void updateParticles();
    /// Feeds the camera position to the scene's GI (PCC probe blending tracks
    /// the viewer in hybrid mode).
    void updateGi();
    /// Releases workspace, camera, workspace definitions and the window/texture.
    /// Safe to call twice. Called by Engine::destroyView and by the Engine
    /// destructor BEFORE Root dies.
    void destroy();

    /// `samples` > 1 asks for an implicit-resolve MSAA target: the sample
    /// description MUST be set before scheduleTransitionTo(Resident) (Ogre
    /// asserts OnStorage); the achieved count is validated at the transition.
    static Ogre::TextureGpu *createRtt(Ogre::Root *root, const std::string &name,
                                       unsigned w, unsigned h, unsigned samples = 1);
    /// Rounds down to a power of two and clamps to [1, 16] — what the backend
    /// will even ask the driver for (the driver may still clamp further).
    static unsigned sanitizeSamples(unsigned samples);

private:
    /// Offscreen only: replaces the RTT (which cannot change in place) at the
    /// given size and mRequestedSamples, re-adding the workspace. The shared
    /// tail of resize() and setSampleCount().
    void rebuildRtt(unsigned w, unsigned h);
    Ogre::TextureGpu *target() const;

    Ogre::Root                *mRoot;
    Ogre::Window              *mWindow;
    Ogre::TextureGpu          *mTexture;
    Ogre::Camera              *mCamera    = nullptr;
    Ogre::CompositorWorkspace *mWorkspace = nullptr;
    OgreScene                 *mScene     = nullptr;
    std::string                mName, mWorkspaceDef, mNodeDef;
    unsigned                   mWidth, mHeight;
    Colour                     mBackground;
    bool                       mEnabled = true;
    bool                       mShadows = false;
    std::string               &mError;
};

// ---------------------------------------------------------------------------
class OgreEngine final : public Engine {
public:
    bool init(const EngineConfig &cfg, std::string &error);

    Scene *createScene(const std::string &name) override;

    void destroyScene(Scene *scene) override;

    View *createView(const std::string &name,
                     NativeWindowHandle handle, unsigned width, unsigned height,
                     const Colour &background) override;

    View *createOffscreenView(const std::string &name, unsigned width, unsigned height,
                              const Colour &background) override;

    void destroyView(View *view) override;

    void renderOneFrame() override;
    const std::string &lastError() const override;

    /// GLOBAL by construction: HlmsPbs keeps ONE ShadowFilter for every shadowed
    /// light in every scene (HlmsPbs::mShadowFilter). The filter properties are
    /// evaluated in preparePassHash each pass, so changing it at runtime takes
    /// effect next frame — no datablock or workspace rebuild.
    void setShadowFilter(ShadowFilter f) override;
    ShadowFilter shadowFilter() const override;

    /// GLOBAL like the filter, but NOT cheap: the sizes live in the shadow-node
    /// DEFINITION, which cannot change while any workspace instantiates it. So:
    /// drop every shadowed view's workspace, swap the definition, re-add them —
    /// the same teardown order the engine's destructor honours.
    void setShadowResolution(unsigned pixels) override;
    unsigned shadowResolution() const override;

    ~OgreEngine() override;

private:
#ifdef __linux__
    struct X11Handle { Display *display; ::Window window; };
#endif

    bool viewNameTaken(const std::string &name);

    /// First render target: the VaoManager now exists, so Hlms can be registered.
    void ensureHlms();
    /// Ogre's low-level material scripts (sky quad, DPSM shadow maps, depth utils).
    /// Staged from Samples/Media/2.0/scripts/materials/Common next to the Hlms data.
    void registerCommonMaterials();
    /// One shadow node for the process: PSSM (3 splits) for the first directional
    /// light and focused maps for the next two point/spot lights, in one atlas.
    /// Mirrors Ogre's ShadowMapFromCode sample. Views opt in with setShadows(true).
    void createShadowNode();

    /// Maps the neutral enum onto HlmsPbs. PCF only: ExponentialShadowMaps is
    /// deliberately NOT used for VerySoft — ESM needs an ESM-compatible shadow
    /// node (colour shadow-map target plus blur passes) which our fixed
    /// depth-atlas shadow node (createShadowNode) is not, and
    /// setShadowSettings(ESM) also flips the global ShadowCameraSetup ESM flag.
    void applyShadowFilter();

    Ogre::Root     *mRoot = nullptr;
    Ogre::Window   *mNullWindow = nullptr;
#ifdef __linux__
    Display        *mDisplay = nullptr;
#endif
    bool            mHlmsRegistered = false;
    ShadowFilter    mShadowFilter = ShadowFilter::Soft;
    unsigned        mShadowResolution = 2048;
    unsigned        mDefaultSamples = 1;   // EngineConfig::sampleCount, sanitized; on-screen views only
    Ogre::AbiCookie mAbiCookie{};
    std::string     mBackendName, mMediaDir, mLastError;
    std::vector<std::unique_ptr<OgreScene>> mScenes;
    std::vector<std::unique_ptr<OgreView>>  mViews;
};

}  // namespace detail
}}  // namespace jahshaka::engine
