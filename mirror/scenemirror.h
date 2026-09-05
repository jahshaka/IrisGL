#ifndef SCENEMIRROR_H
#define SCENEMIRROR_H

// SceneMirror — pushes the iris:: scene DOCUMENT into an engine::Scene.
//
// This is the seam decided in VIEWPORT_MIGRATION_PLAN.md: Studio keeps
// iris::Scene/SceneNode/MeshNode/LightNode as its document model (the property
// panels, hierarchy widget, undo commands, reader/writer all talk to it) and the
// engine renders a mirror of it. Every frame sync() walks the document, creates
// engine nodes for new document nodes, removes engine nodes for vanished ones,
// and pushes local transforms and visibility. Meshes are converted once from the
// document's CPU vertex buffers and cached per iris::Mesh.
//
// The document→engine bridge — the ONE component that knows both sides
// (document types AND the engine abstraction). Lives in the IrisGL repo
// (audit §3.3/§10: iris/mirror/) so boundary changes and mirror updates land
// in one commit. Includes iris (Qt) and jahshaka/engine. Never Ogre.
#include "core/math/mat4.h"
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QSet>
#include <utility>
#include <vector>
#include "irisgl/irisglfwd.h"
#include "irisgl/document/animation/clipextractor.h"
#include "irisgl/document/scenegraph/socket.h"
#include "jahshaka/engine/Engine.h"

// The graph-ownership handle (nodegraph.h's opaque pair) — forward-declared so
// this header stays free of the graph API.
namespace iris { namespace graph {
struct SceneOpaque;
using SceneHandle = SceneOpaque *;
}}

namespace iris { class Mesh; class Material; struct SkyRealistic; }

class SceneMirror
{
public:
    explicit SceneMirror(jahshaka::engine::Scene *target);
    ~SceneMirror();

    /// Replaces the mirrored document. Clears everything previously mirrored.
    void setSource(iris::ScenePtr scene);
    iris::ScenePtr source() const { return mSource; }

    /// Brings the engine scene up to date with the document. Call once per frame
    /// before Engine::renderOneFrame(). Returns the number of document nodes mirrored.
    int sync();

    /// The engine node mirroring a document node, or 0.
    jahshaka::engine::NodeId engineNode(const iris::SceneNode *node) const;

    /// Points `view`'s camera where the document camera is looking.
    void applyCamera(iris::CameraNodePtr camera, jahshaka::engine::View *view);
    /// Points `view`'s picture-in-picture inset at a document camera
    /// (CAMERAS_SPEC D3, phase 2c). `desc` carries everything but the camera —
    /// the rect, the inset's background, the offscreen opt-in — and this fills
    /// in `desc.camera` from the document node, the same translation
    /// applyCamera does for the main view.
    ///
    /// A null camera or `desc.enabled == false` switches the inset OFF, which
    /// the engine guarantees is byte-exact (no workspace, no trace). Cheap to
    /// call every frame: an unchanged value never reaches the backend.
    void applyPip(iris::CameraNodePtr camera, jahshaka::engine::View *view,
                  const jahshaka::engine::ViewPipDesc &desc);

    /// Pushes the document's sky onto the view. Flat colour skies become the clear
    /// colour; cubemap/equirect/gradient/realistic skies are a later step and leave
    /// the view's current background.
    void applySky(jahshaka::engine::View *view);

    /// Pushes the document's world settings the sky doesn't cover: ambient colour
    /// (flat, like the legacy uniform), the scene's shadowEnabled toggle, fog, and
    /// — when `engine` is given — the shadow filter quality. The engine's filter is
    /// GLOBAL (one per process, Engine::setShadowFilter) while the document stores
    /// a per-light ShadowMapType, so the policy is: push the strongest (softest)
    /// quality requested by any shadow-casting light, computed by the last sync().
    void applyEnvironment(jahshaka::engine::View *view,
                          jahshaka::engine::Engine *engine = nullptr);

    /// Forgets what applyEnvironment has already pushed, so the next call pushes
    /// everything again.
    ///
    /// applyEnvironment debounces ambient / fog / GI against the last value it
    /// sent, which is right while ONE mirror owns the screen — but some of that
    /// state is process-wide inside the backend (HlmsPbs' VCT/PCC binding is
    /// literally "last scene to enable owns it", OgreGi.cpp). The editor and the
    /// player are two mirrors over two engine scenes taking turns on screen: the
    /// one coming back would otherwise decide it had already pushed and leave the
    /// other's binding in place. Call this whenever a mirror (re)takes the
    /// screen — EngineSceneViewport::begin(), EnginePlayerScene::begin().
    void invalidateEnvironment();

    /// How many times applyEnvironment has pushed a NEW GI configuration
    /// (Scene::setGlobalIllumination) and how many times it has asked for a
    /// re-solve of the existing one (Scene::refreshGlobalIllumination).
    ///
    /// Both are expensive — a VCT refresh tears the voxelizer down and rebuilds
    /// it from every item in the scene — and both are debounced against the last
    /// pushed value, so "an idle scene refreshes ZERO times" is a contract, not
    /// an optimisation. It is not observable from the document or from pixels
    /// (a re-voxelized scene looks identical; it just costs a frame), which is
    /// why the counters exist: mirror.document_to_engine's "GI idle" block
    /// asserts an idle VCT scene re-solves ZERO times over 60 frames, and that
    /// a light that really moves re-solves exactly once.
    quint64 giPushCount() const { return mGiPushCount; }
    quint64 giRefreshCount() const { return mGiRefreshCount; }

    /// The document light node driving Instant Radiosity: the scene's giLightGuid
    /// when it names a live light, else the first directional light (by creation
    /// order), else any light. Null when the scene has no lights. Public so the
    /// world panel can show which light "Automatic" resolves to.
    iris::LightNode *resolveGiLight() const;

    /// Converts a document mesh to engine MeshData. Public so importers and tests
    /// can use the same conversion. Returns false if the mesh has no geometry.
    static bool toMeshData(iris::Mesh *mesh, jahshaka::engine::MeshData &out);
    /// Extracts the mesh's per-vertex bone data (4 float indices + 4 float weights
    /// per vertex, the layout the legacy GL skinning shader consumed). False when
    /// the mesh carries no bone buffers. Public for tests.
    static bool toSkinData(iris::Mesh *mesh, std::vector<float> &boneIndices,
                           std::vector<float> &boneWeights);
    /// Translates a document skeleton into the engine's rig descriptor
    /// (GPU_SKINNING_SPEC §5 + R1). Bone ORDER is preserved — the index is what
    /// the mesh's per-vertex blend indices name — and each bone's bind transform
    /// is authored so that the engine's derived reverse bind pose comes out
    /// EXACTLY equal to assimp's offset matrix, which is what makes the engine's
    /// world-relative skinning and the document's mesh-node-relative skin
    /// matrices the same maths. `id` is a hash of the bone STRUCTURE only (names,
    /// hierarchy, bind transforms) — never the source file, never the clip set:
    /// the engine's rig cache is process-wide and keyed on it, so two files of
    /// one rig must resolve to one entry. False for a null or empty skeleton.
    /// Static and public for tests.
    static bool toSkeletonDesc(const iris::SkeletonPtr &skeleton,
                               jahshaka::engine::SkeletonDesc &out);
    // toBonePoses is GONE with the document's clip evaluator: there is no
    // document-computed pose to convert any more. The engine holds the pose;
    // Scene::bonePoses reads it back (and boneWorldTransforms below turns that
    // into the world matrices the bone overlay draws).
    /// An extracted clip (iris::ClipExtractor) as an engine ClipDesc.
    ///
    /// The `id` is a CONTENT hash of the rig id and every key of every track,
    /// and it has to be: the backend caches the translated clip under it for the
    /// life of the process, keyed by name, so an id derived from a file path or
    /// an asset guid makes a re-imported clip alias the old one forever.
    /// Static and public for tests.
    static bool toClipDesc(const iris::ExtractedClip &clip, const std::string &rigId,
                           jahshaka::engine::ClipDesc &out);
    /// CPU-skins bind-pose vertices with a set of bone matrices — exactly the
    /// legacy GL shader's math (weighted sum of bone matrices).
    /// bindNormals/outNormals may be empty. Static and public for tests.
    ///
    /// NOT PART OF RENDERING (GPU_SKINNING_SPEC §6): skinning happens in the
    /// vertex shader and sync() never calls this. It survives as the ORACLE the
    /// GPU path is checked against (skeletal.gpu_parity), and it survived the
    /// clip evaluator's retirement because it is not an evaluator: it is a pure
    /// function of (bone matrices, vertices), and the matrices now come from the
    /// ENGINE (Scene::boneMatrices) instead of from the document. Note it skins
    /// position and normal only — the shader also skins the TANGENT, so a
    /// normal-mapped character lights correctly on the GPU path and did not on
    /// this one.
    static void skinVertices(const QVector<iris::Mat4> &boneTransforms,
                             const std::vector<float> &bindPositions,
                             const std::vector<float> &bindNormals,
                             const std::vector<float> &boneIndices,
                             const std::vector<float> &boneWeights,
                             std::vector<float> &outPositions,
                             std::vector<float> &outNormals);
    /// Every skinned bone's WORLD matrix, by bone name, read back from the
    /// ENGINE — the only place a pose exists once the document's clip evaluator
    /// is retired.
    ///
    /// The engine returns each bone LOCAL to its parent (a root bone: local to
    /// the mesh node), so this runs the same FK the shader's bone matrices came
    /// from and premultiplies the mesh node's global transform. That is what the
    /// bone overlay and `avatar.bones` draw, and it is resolved as of the last
    /// rendered frame — call it after sync() and after a render, never before.
    /// False when nothing skinned is mirrored.
    bool boneWorldTransforms(QHash<QString, iris::Mat4> &out) const;
    /// The same, for ONE node — which is what sockets need, because bone names
    /// are only unique within a rig and the whole-scene overload above silently
    /// lets a second character of the same rig overwrite the first's bones.
    /// False when the node is not a mirrored skinned mesh.
    bool boneWorldTransforms(iris::SceneNode *node, QHash<QString, iris::Mat4> &out) const;
    /// Drives every socket-attached node of the source scene from the pose the
    /// last rendered frame produced (CAMERAS_SPEC §5). Called at the top of
    /// sync(); public so a headless suite can step it explicitly. Returns how
    /// many nodes moved.
    int resolveSockets();
    /// The socket resolver, for tests and for hosts that want the dangling
    /// count. Its pose source is installed by this mirror's constructor.
    iris::SocketResolver &socketResolver() { return mSockets; }
    /// Pushes a world matrix onto an engine node as TRS (used by overlays too).
    static void pushTransform(jahshaka::engine::Scene *scene, jahshaka::engine::NodeId node, const iris::Mat4 &world);
    /// The engine mesh already created for a document mesh, or 0.
    jahshaka::engine::MeshId engineMesh(iris::Mesh *mesh) const;

    /// Selection highlight: the node's mesh drawn again as an on-top wireframe.
    void setHighlightedNode(iris::SceneNodePtr node);

    /// Selection highlight look: false (default) = silhouette outline (inverted
    /// hull); true = the on-top polygon wireframe.
    void setHighlightWireframe(bool on);
    bool highlightWireframe() const { return mHighlightWireframe; }
    /// Light helpers: an icon billboard (sun/bulb/spotlight) at every document
    /// light, plus a wire shape in the light's colour. The attenuation volume
    /// (point rings / spot cone, sized by the light's range) shows only for the
    /// HIGHLIGHTED light — the Unreal convention — while the direction arrow
    /// (directional/spot) and the area rectangle (the light's physical shape)
    /// stay on for every light whenever helpers are enabled.
    void setLightWires(bool on);
    bool lightWires() const { return mLightWires; }

    /// Camera helpers (CAMERAS_SPEC D2, phase 2b): a small camera BODY and its
    /// view FRUSTUM, drawn as unlit on-top lines at every scene CameraNode whose
    /// `bodyVisible` is set. Highlighted like any other helper when the camera
    /// is the selected node.
    ///
    /// Separate from setLightWires on purpose — the two are different objects
    /// with different toggles — but hosts hide BOTH in Game View and in play,
    /// which is where "editor helper" is actually defined. The engine keeps
    /// them out of a picture-in-picture inset by RENDER QUEUE (they are on-top
    /// overlays, and the inset draws below that range), so a camera never
    /// appears in its own preview.
    void setCameraBodies(bool on);
    bool cameraBodies() const { return mCameraBodies; }

    /// Editor ground grid (EDITOR_SHORTCUTS_SPEC §3): an unlit line overlay on
    /// y=0 — extent ±100 units, a line every `spacing`, every 10th line major
    /// (brighter). Same never-fogged overlay class as the light wires, depth-
    /// tested so geometry occludes it. Hidden by default; hosts push visibility
    /// and spacing per frame (cheap — the mesh only rebuilds when the spacing
    /// changes). The next sync() applies it.
    void setGrid(bool visible, float spacing);
    bool gridVisible() const { return mGridVisible; }

    /// How far the grid reaches from the origin, in world units (default 100 =
    /// the editor's ±100 floor). A preview whose subject is a 170-unit-tall
    /// character needs a bigger one, or the "floor" is smaller than the thing
    /// standing on it. Changing it rebuilds the grid meshes on the next sync.
    void setGridExtent(float extent);
    /// Grid line colours (minor, major). Alpha is the line's opacity. The
    /// editor keeps its blue-grey default; the avatar preview asks for white.
    void setGridColours(const jahshaka::engine::Colour &minor,
                        const jahshaka::engine::Colour &major);

    /// The legacy Preetham "realistic" sky, CPU-baked to an equirect image —
    /// exactly realisticsky.frag's math per direction. Public for tests.
    /// CPU bake of the analytic (Preetham) sky into an equirect image.
    ///
    /// `forHdr` is the POST_CHAIN_SPEC §7.1 decision, adopted: the bake normally
    /// applies its own Uncharted2 filmic curve and a gamma, because the result
    /// goes straight to an LDR viewport. Feed THAT into an HDR chain and the sky
    /// is tonemapped TWICE — washed-out, low-contrast skies in exactly the
    /// scenes that look best today. With `forHdr` the bake stops after the
    /// exposure and lets the chain's tonemapper do the grading, once.
    static QImage bakeRealisticSky(const iris::SkyRealistic &sky, int width, int height,
                                   bool forHdr = false);

    /// Cosine-convolved irradiance of an equirect sky image as 9 spherical-
    /// harmonic bands (27 floats, r/g/b per band), in LINEAR light — what the
    /// scene's ambient becomes when `Scene::ambientFromSky` is on
    /// (VISUAL_PARITY_SPEC item 3b). Basis, order and units are exactly what
    /// `Scene::setAmbientSh` documents; row 0 of the image is the zenith and the
    /// longitude follows Ogre's own sky shader. Returns false for a null image.
    /// Public for tests.
    static bool integrateSkyAmbientSh(const QImage &equirect, float shOut[27]);

private:
    struct Entry {
        jahshaka::engine::NodeId node = 0;
        /// The DOCUMENT's Ogre scene node this entry adopted (opaque —
        /// iris::graph::NodeHandle). Compared every sync: a migration between
        /// scene managers rebuilds the handle, and the adopted id then names a
        /// node that no longer exists.
        const void *graphNode = nullptr;
        /// ...and its epoch. The pointer alone is not enough: Ogre recycles
        /// node memory, so a migration out of this scene manager and back can
        /// hand the rebuilt node the SAME address.
        quint32 graphEpoch = 0;
        /// The visibility last pushed; -1 = never. Visibility is the document's
        /// flag (Ogre's setVisible walks a node's attachments, so an empty node
        /// has no visibility of its own) but it is pushed on CHANGE only.
        int visiblePushed = -1;
        /// The `pickable` flag last pushed onto this node's engine objects as
        /// Ogre QUERY FLAGS; -1 = never. Ogre's RaySceneQuery is the picking
        /// broad phase now (SCENEGRAPH_SPEC §2), and its mask is tested inside
        /// the SIMD sweep — so unpickable geometry has to carry the bit that
        /// keeps it out. Change-guarded like visibility, and re-pushed whenever
        /// geometry is (re-)attached, because the flags live on the Item and a
        /// new Item is born with the default mask.
        int pickablePushed = -1;
        /// The sync() this entry was last reached by. See mSyncStamp.
        quint32 lastSeen = 0;
        bool hasMesh  = false;
        bool hasLight = false;
        bool hasDecal = false;                       // an engine decal is bound
        quint64 decalSignature = 0;                  // image guid+path+kind set; re-bind on change
        jahshaka::engine::MaterialId material = 0;   // per document material instance
        iris::Material *materialPtr = nullptr;
        jahshaka::engine::MeshId mesh = 0;           // shared engine mesh this entry uses
        iris::Mesh *meshPtr = nullptr;
        /// The PBR state last pushed for `material`, and whether anything was.
        ///
        /// setPbrMaterial is NOT free: it re-applies the whole datablock, which
        /// schedules a const-buffer upload, and Ogre's setTwoSidedLighting used
        /// to flush every renderable's Hlms hash on top (deep audit 2026-09,
        /// area 5 — the engine guards that now too). The property panel can
        /// change these values any frame, so the mirror still has to LOOK every
        /// frame; it just does not have to PUSH.
        jahshaka::engine::PbrParams lastPbr;
        bool pbrPushed = false;
        quint64 textureSignature = 0;                // which files are bound; re-sync on change
        bool texturesPushed = false;                 // ...and whether anything was ever pushed
        /// The engine texture ids this entry's material has bound right now, so
        /// reclaimUnused can free the ones no live entry references any more.
        /// The signature alone cannot do it: it says WHICH FILES, not which ids,
        /// and a reclaim needs ids.
        std::vector<jahshaka::engine::TextureId> boundTextures;
        jahshaka::engine::NodeId wireNode = 0;       // light wire shape, child of `node`
        jahshaka::engine::MaterialId wireMaterial = 0;
        int wireKind = -1;                           // which shape is attached
        /// Last colour pushed to `wireMaterial`. A light's wire colour changes
        /// only when the user edits the light, but the push happened every
        /// frame, and setUnlitMaterial schedules a const-buffer update per call.
        jahshaka::engine::Colour wireColour;
        bool wireColourPushed = false;
        bool hasIcon = false;                        // light icon billboard on wireNode
        QString iconSignature;                       // icon image path; recreate on change
        /// Camera body + frustum (CAMERAS_SPEC phase 2b). The mesh is OWNED by
        /// this entry — it is derived from the camera's own lens, so there is
        /// nothing to share — and rebuilt only when `cameraSignature` moves.
        jahshaka::engine::MeshId cameraMesh = 0;
        quint64 cameraSignature = 0;
        // Planar reflections: the last flag pushed to the engine, so the
        // per-frame visit does nothing when nothing changed. -1 = never pushed.
        // Arming a reflector derives a world plane and registers a PBS
        // receiver; it is not the kind of call to repeat 60 times a second.
        int planarReflector = -1;
        // (hasBillboards/billboardSignature lived here and had no reader or
        // writer anywhere in the tree — deleted with the deep-audit fix wave.)
        // Particles (PARTICLES_FX2_SPEC): the engine simulates, so the mirror
        // pushes PARAMETERS, not particles — and only when they change. The
        // signature covers every authored value, so a still emitter costs one
        // 64-bit hash per sync and no engine call at all. (Before the adoption
        // this branch rebuilt a std::vector<BillboardInstance> of every live
        // particle, sixty times a second; before the deep-audit fix wave the
        // signature itself was a QString built through QTextStream — an
        // allocation and a locale-aware float format per emitter per frame,
        // under a comment that claimed it allocated nothing.)
        bool hasParticles = false;
        quint64 particleSignature = 0;
        /// The map the engine's particle definition is holding. Comes out of
        /// the shared texture cache, so reclaimUnused has to see it.
        jahshaka::engine::TextureId particleTexture = 0;
        // Skinning (GPU_SKINNING_SPEC): the NODE's own skeleton, not the mesh
        // asset's shared rig template. Pose state is per node, so two duplicates
        // of one character animate independently — on the GPU each node's Item
        // carries its own SkeletonInstance, so they also LOOK different.
        iris::SkeletonPtr skeleton;
        bool gpuSkinned = false;                     // the engine accepted the rig
        size_t boneCount = 0;
        // Clip playback (ANIMATION_ENGINE_MIGRATION_SPEC M3). The document says
        // WHICH clip and WHEN; the engine samples and blends it.
        iris::SceneNode *docNode = nullptr;          // the document node this entry mirrors
        std::string rigId;                           // for the clip def's content key
        QString clipSignature;                       // rig + clip set; re-attach on change
        /// Document clip -> its content id -> the name the engine gave it.
        ///
        /// Two hops because attachClips is IDEMPOTENT PER CONTENT ID and clips
        /// really do collide: the Avatar page hands setAnimation a rebuilt copy
        /// of a clip (root motion is a preview policy), and when there is no
        /// root motion to strip the copy is byte-identical, so the engine keeps
        /// one def for both. A one-hop map would leave the copy — the one that
        /// is actually PLAYING — with no engine clip and the character frozen.
        ///
        /// Keyed by the AnimationPtr's IDENTITY, not by name and not by index.
        /// Not by index because clips ACCUMULATE on a node (the Avatar page
        /// loads a Mixamo animation onto a loaded character) and an index would
        /// shift under them; not by NAME because clip names are not unique —
        /// every Mixamo clip is literally called "mixamo.com", so a character
        /// with its own T-pose plus a downloaded walk has two clips of that name
        /// and a name lookup silently plays the wrong one.
        QHash<const iris::Animation *, QString> clipMap;      // animation -> clip id
        QHash<QString, QString> clipIdMap;                    // clip id -> engine clip name
        QString  lastClipName;                       // last state pushed, to skip no-ops
        float    lastClipTime = -1.0f;
        bool     lastClipLooping = false;
    };
    /// Pushes a ParticleSystemNode's AUTHORING parameters into the engine
    /// (PARTICLES_FX2_SPEC §5), which then simulates them. Guarded by a
    /// signature over every authored value: an unchanged emitter costs no
    /// engine call at all.
    void syncParticles(Entry &e, iris::ParticleSystemNode *ps);
    void syncLightWires(Entry &e, iris::LightNode *light);
    /// Colours an entry's wire material, on change only.
    void pushWireColour(Entry &e, const jahshaka::engine::Colour &c);
    /// Pushes a DecalNode into the engine (DECALS_SPEC §5.3) and drives its
    /// wire box. A decal whose image is missing or whose atlas is full leaves
    /// the node decal-free — the wire box still draws, so the user sees the
    /// object exists and the panel can say why it projects nothing.
    void syncDecal(Entry &e, iris::DecalNode *decal);
    void syncDecalWires(Entry &e, iris::DecalNode *decal);
    /// A decal image goes into the DEDICATED atlas, never the ordinary texture
    /// cache: loadTexture()'s pool-0/grayscale paths are both unusable there.
    jahshaka::engine::TextureId decalTextureFor(const QString &path,
                                                jahshaka::engine::DecalMap kind);
    void syncLightIcon(Entry &e, iris::LightNode *light);
    /// Builds/updates a camera's body + frustum lines (CAMERAS_SPEC §3). The
    /// geometry is DERIVED — fov, aspect, near and the clipped far all come out
    /// of the document — so unlike the light shapes it cannot be one cached
    /// mesh per kind: each camera owns a mesh, rebuilt only when the signature
    /// of those values (and the selection state) changes.
    void syncCameraWires(Entry &e, iris::CameraNode *camera);
    jahshaka::engine::TextureId iconTextureFor(const QString &path);
    void syncHighlight();
    void syncGrid();
    jahshaka::engine::MeshId wireMeshFor(int kind);
    /// The per-sync document walk. Takes a RAW node and iterates children
    /// through iris::graph rather than through SceneNode::children(), which
    /// materialises a QList<QSharedPointer> — one heap allocation and one
    /// atomic refcount per child — for every node of the scene, every frame.
    void visit(iris::SceneNode *node);
    void releaseEntry(Entry &e);
    /// Releases every engine object this mirror hung off DOCUMENT nodes
    /// (entries + highlight shells) — the body of the graph-evacuation hook
    /// (Scene::_setGraphEvacuationHook). Runs while the nodes still exist,
    /// right before another mirror migrates the document's graph away. The
    /// mirror-scene-scoped objects (grid, sky, wire meshes) stay: they live on
    /// OUR engine scene's own nodes, not the document's.
    void evacuateEngineObjects();
    /// The graph handle of OUR engine scene as of the last bind — kept so the
    /// destructor can test ownership without touching mTarget (which may
    /// already be gone).
    iris::graph::SceneHandle mBoundHandle = nullptr;
    /// Drops the entries this sync did not reach. The "seen" set used to be a
    /// QSet<long> filled with one insert per node per frame (and keyed on a
    /// type that is 32-bit on Windows LLP64 while nodeId is 64-bit — audit F7);
    /// it is a stamp on the entry now, so noticing a node costs a store.
    void removeMissing();
    /// Frees engine meshes/materials no live entry references (asset browsing would
    /// otherwise grow them for the life of the process; pointer keys could alias).
    void reclaimUnused();
    /// Per-frame animation, all of it (ANIMATION_ENGINE_MIGRATION_SPEC M3): for
    /// every GPU-skinned node, attach its clips ONCE (translated out of the
    /// document's scene-node channels by iris::ClipExtractor, which composes any
    /// `$AssimpFbx$` pivot chain away) and then push nothing but
    /// {which clip, absolute time, looping} per frame.
    ///
    /// This replaced the old syncBonePoses, which decomposed the document's skin
    /// matrices to per-bone TRS every frame. The document no longer computes a
    /// pose at all — it states the clip and the clock, and Ogre's threaded SIMD
    /// FK does the rest.
    void syncClips();
    /// One entry's bones in world space, APPENDED to `out` (it is not cleared —
    /// the whole-scene overload accumulates every rig into one map). False when
    /// the entry is not a posed skinned mesh.
    bool entryBoneWorldTransforms(const Entry &e, QHash<QString, iris::Mat4> &out) const;
    /// Translates and attaches a node's clips. Idempotent: does nothing unless
    /// the rig or the clip set changed.
    void attachClipsFor(Entry &e);
    /// Resamples an equirect sky image into six small cubemap faces and pushes
    /// them as the scene's environment reflections (Scene::setSkyReflection) —
    /// how equirect/gradient/realistic skies get the IBL cubemap skies have.
    /// Also records the sky's ambient integral for applyEnvironment (item 3b).
    void applySkyReflection(const QImage &equirect);
    /// Cubemap skies do not go through applySkyReflection (the engine takes the
    /// six faces directly), so their ambient integral is taken from the face
    /// images: the same SH projection, per face texel.
    void recordCubeAmbientSh(const QImage faces[6]);
    /// Clears the recorded sky ambient (no sky, or a single-colour sky).
    void clearSkyAmbient();
    jahshaka::engine::MeshId     meshFor(iris::Mesh *mesh);
    jahshaka::engine::MaterialId materialFor(iris::Material *material);
    void syncTextures(Entry &e, iris::Material *material);
    jahshaka::engine::TextureId textureFor(const QString &path, bool srgb);
    /// Reads a document material into PBR parameters. Public for tests.
public:
    static bool toPbrParams(iris::Material *material, jahshaka::engine::PbrParams &out);
    /// Records that this material is refractive, for the chain's Auto mode.
    void noteRefractive(const jahshaka::engine::PbrParams &p);
    static jahshaka::engine::LightDesc toLightDesc(iris::LightNode *light);
    /// Fills everything but the texture ids (those need the atlas).
    static jahshaka::engine::DecalDesc toDecalDesc(iris::DecalNode *decal);
    /// The document -> engine particle mapping (PARTICLES_FX2_SPEC §5), isolated
    /// so the suites can assert on the desc without a scene or a frame. Static
    /// for the same reason toLightDesc is: it reads the node and nothing else.
    static jahshaka::engine::ParticleSystemDesc toParticleDesc(
        iris::ParticleSystemNode *ps, jahshaka::engine::TextureId tex);
private:

    jahshaka::engine::Scene *mTarget;
    iris::ScenePtr           mSource;
    /// Keyed by the DOCUMENT NODE ITSELF, not by `nodeId` (SCENEGRAPH_SPEC §4:
    /// "the mirror's ... nodeId entry keying" dies with the swap). The node
    /// pointer is the identity the walk already holds, so this is one pointer
    /// hash instead of a field read plus a 64-bit hash — and the freed-address
    /// hazard shared with mMeshes below is closed the same way plus one more:
    /// every entry carries the engine node's handle and epoch, so an entry
    /// reached through a recycled address is released and re-adopted rather
    /// than believed.
    QHash<const iris::SceneNode *, Entry> mEntries;
    /// Bumped once per sync(); an entry whose `lastSeen` is not this value did
    /// not appear in the document this frame. Replaces the per-frame seen-set.
    quint32                  mSyncStamp = 0;
    /// How many document nodes the current walk reached — sync()'s return value.
    int                      mVisited = 0;
    /// Something may have stopped referencing an engine mesh / material /
    /// texture since the last cache sweep. reclaimUnused() rebuilds three
    /// QSets out of every entry in the scene, so running it on a frame where
    /// nothing was released is pure cost — 20k+ set inserts a frame at 10k
    /// nodes. Armed by releaseEntry() and by every site that CHANGES an
    /// entry's mesh / material / texture / particle-texture reference.
    /// Conservative by construction: a missed site delays a free to the next
    /// real change, it never frees something still in use.
    bool                     mReclaimPending = true;
    /// PER-SYNC memo of the two things that depend only on the MATERIAL, not on
    /// the node: its PbrParams and its texture-bind signature. Both used to be
    /// recomputed per MESH per frame — `toPbrParams` runs two dynamic_casts and
    /// a scan of every shader property, and `syncTextures` did seven
    /// QHash<QString> lookups whose keys it built from `const char *` (a QString
    /// construction each) plus a QVector of binds — so a scene of 8000 cubes
    /// sharing ONE material paid for that material 8000 times a frame. Cleared
    /// at the top of every sync: within one sync a material cannot change.
    struct TextureBind {
        jahshaka::engine::PbrTextureSlot slot;
        QString path;
        bool srgb;
    };
    struct MaterialSync {
        bool                          hasPbr = false;
        jahshaka::engine::PbrParams   pbr;
        quint64                       textureSignature = 0;
        std::vector<TextureBind>      binds;
    };
    QHash<iris::Material *, MaterialSync> mMaterialSync;
    const MaterialSync &materialSyncFor(iris::Material *material);
    /// Socket attachments (CAMERAS_SPEC §5). Owns the reused scratch buffers;
    /// its pose source is this mirror, installed by the constructor.
    iris::SocketResolver     mSockets;
    /// entryBoneWorldTransforms' scratch. Members because sockets made that
    /// function per-frame work (it used to run only when the bone overlay
    /// refreshed) — see the note at its assign() calls.
    mutable std::vector<jahshaka::engine::BonePose> mPoseScratch;
    mutable std::vector<iris::Mat4>                 mDerivedScratch;
    mutable std::vector<char>                       mDerivedDone;
    /// Document mesh/material -> engine object, keyed by RAW POINTER.
    ///
    /// KNOWN RESIDUAL (deep audit 2026-09, area 5, deliberately not fixed here):
    /// the allocator can hand a freed iris::Mesh's address to a new one, and a
    /// stale entry would then alias the wrong engine mesh. reclaimUnused runs
    /// every sync() and erases any entry no live Entry references, which closes
    /// the window to "a mesh freed and a new one allocated at the same address
    /// between two syncs, while the old one was still referenced" — i.e. it
    /// cannot happen through the mirror's own bookkeeping. Closing it properly
    /// needs a stable identity ON iris::Mesh/iris::Material (a monotonic
    /// generation counter or the asset guid); neither type has one today, and
    /// adding one is a document-model change, not a mirror change.
    QHash<iris::Mesh *, jahshaka::engine::MeshId> mMeshes;
    // (A second `mPoseScratch` lived here, unused since the document's pose PUSH
    // was retired with its clip evaluator — deleted rather than shadowed by the
    // read-back scratch above, which is a live buffer with the same name.)
    QHash<iris::Material *, jahshaka::engine::MaterialId> mMaterials;
    QHash<QString, jahshaka::engine::TextureId> mTextures;
    QHash<QString, jahshaka::engine::TextureId> mIconTextures;   // light icon glyphs (Qt resources)
    jahshaka::engine::MaterialId mDefaultMaterial = 0;
    bool mLightWires = true;
    bool mCameraBodies = true;
    /// The camera currently DRIVING the view (applyCamera's last one, after the
    /// active-camera substitution). Its own body and frustum are suppressed —
    /// see applyCamera. Raw pointer, compared only for identity; the document
    /// owns it and a dangling value can only ever fail to match.
    const iris::CameraNode *mViewCamera = nullptr;
    /// Which sky the engine currently shows, and a 64-bit hash of the values it
    /// was built from. Two fields rather than one string because applySky
    /// DISPATCHES on the kind (and the realistic-bake debounce asks "was the
    /// previous sky also realistic?"), while the parameters only ever need an
    /// equality test — and building the parameter string cost ten QString::arg
    /// calls per frame to conclude nothing had changed.
    enum class SkyKind { None, Equirect, Cubemap, Gradient, Realistic };
    SkyKind mSkyKind = SkyKind::None;
    quint64 mSkyHash = 0;
    /// The equirect sky's texture, taken from the shared cache (unlike the
    /// cubemap/gradient/realistic paths, which upload their own). Held so
    /// reclaimUnused does not free what the engine's sky is sampling.
    jahshaka::engine::TextureId mSkyTexture = 0;
    jahshaka::engine::TextureId mSkyFaceTextures[6] = { 0, 0, 0, 0, 0, 0 };
    // Faces the reflection (IBL) cubemap was built from; kept until the sky
    // changes (the engine copies them, but destroy-after-copy stays ours).
    jahshaka::engine::TextureId mReflFaceTextures[6] = { 0, 0, 0, 0, 0, 0 };
    // Realistic-sky bake debounce: during a slider drag the 8 parameters change
    // every event; re-bake at most every ~150 ms (the last change always lands —
    // applySky recomputes the signature each frame until it sticks).
    QElapsedTimer mRealisticBakeTimer;
    // Sky-driven ambient (VISUAL_PARITY item 3b): the cosine-weighted hemisphere
    // integrals of whatever sky is live, in linear light. Recomputed only when
    // the sky signature changes; applyEnvironment pushes them (or the flat
    // document colour when there is no sky, or the scene opts out).
    bool mHasSkyAmbient = false;
    // Last ambient pair actually pushed. Ogre picks its ambient shader variant
    // from these (equal => fixed, different => hemisphere), so pushing an
    // unchanged value every frame is not free.
    bool mAmbientPushed = false;
    bool mLastAmbientWasSky = false;
    jahshaka::engine::Colour mLastFlatAmbient { -1.0f, -1.0f, -1.0f, 1.0f };
    float mLastAmbientSh[27] = { 0.0f };
    /// The sky's own SH ambient (before the World-panel gain). Valid while
    /// mHasSkyAmbient; zeroed by clearSkyAmbient.
    float mSkyAmbientSh[27] = { 0.0f };
    // directional, point, spot, area, decal box
    jahshaka::engine::MeshId mWireMeshes[5] = { 0, 0, 0, 0, 0 };
    /// Decal image path + map kind -> pooled atlas slice id. Separate from
    /// mTextures on purpose: the two live in different pools and a mix-up is
    /// silent (the decal would sample another decal's image).
    QHash<QString, jahshaka::engine::TextureId> mDecalTextures;
    // Ground grid: one root node (dropped a hair below y=0 against z-fighting
    // with floor geometry) carrying a minor- and a major-line child.
    bool  mGridVisible = false;
    float mGridSpacing = 1.0f;
    float mGridExtent = 100.0f;
    float mGridBuiltSpacing = -1.0f;                            // what the meshes were built for
    float mGridBuiltExtent = -1.0f;
    jahshaka::engine::Colour mGridMinorColour{ 0.46f, 0.48f, 0.52f, 0.28f };
    jahshaka::engine::Colour mGridMajorColour{ 0.62f, 0.64f, 0.68f, 0.50f };
    bool  mGridColoursDirty = false;
    jahshaka::engine::NodeId mGridNode = 0, mGridMinorNode = 0, mGridMajorNode = 0;
    jahshaka::engine::MeshId mGridMinorMesh = 0, mGridMajorMesh = 0;
    jahshaka::engine::MaterialId mGridMinorMaterial = 0, mGridMajorMaterial = 0;
    iris::SceneNodePtr mHighlighted;
    /// One highlight shell per mesh under the highlighted node: selecting an
    /// asset's root outlines the whole subtree. Pooled and reused across frames.
    struct HighlightShell {
        jahshaka::engine::NodeId node = 0;
        jahshaka::engine::MeshId mesh = 0;   // engine mesh currently attached
        bool wireframe = false;              // which material the shell carries
    };
    std::vector<HighlightShell> mHighlightShells;
    void collectHighlightMeshes(const iris::SceneNodePtr &node,
                                std::vector<std::pair<iris::MeshNode *, jahshaka::engine::MeshId>> &out);
    jahshaka::engine::MaterialId mHighlightMaterial = 0;   // wireframe (on top)
    jahshaka::engine::MaterialId mOutlineMaterial = 0;     // inverted hull
    bool mHighlightWireframe = false;
    QColor mHighlightColourApplied;                        // what the materials show now
    // Strongest shadow quality any shadow-casting light asked for, from the last
    // sync(); pushed engine-wide by applyEnvironment (see comment there).
    jahshaka::engine::ShadowFilter mShadowFilter = jahshaka::engine::ShadowFilter::Hard;
    bool mAnyShadowCaster = false;
    /// Does the scene contain a refractive material right now? Drives the
    /// chain's "Auto" refraction mode (POST_CHAIN_SPEC.md §9.5); recomputed
    /// every sync() exactly like mAnyShadowCaster.
    bool mAnyRefractive = false;
    // Largest shadow-map resolution any shadow-casting light asked for, from the
    // last sync(); pushed engine-wide by applyEnvironment (the engine's atlas is
    // global, like the filter — rebuild is expensive, so only on change).
    unsigned mMaxShadowResolution = 0;
    // Global illumination: last pushed state + the driving light's transform, so
    // applyEnvironment only re-pushes on change and re-traces on light movement.
    jahshaka::engine::GiParams mLastGi;
    bool mGiPushed = false;
    iris::Mat4 mGiLightWorld;
    // Diagnostics behind giPushCount() / giRefreshCount(); never read by the
    // mirror itself, and NOT reset by invalidateEnvironment (a re-take of the
    // screen is a real push, and the gate counts real pushes).
    quint64 mGiPushCount = 0;
    quint64 mGiRefreshCount = 0;
    // Fog: last pushed state. Enabling/disabling fog creates or destroys the
    // scene's atmosphere and changes the shader variant, so this one is pushed on
    // change only, not every frame.
    jahshaka::engine::FogDesc mLastFog;
    bool mFogPushed = false;
};

#endif // SCENEMIRROR_H
