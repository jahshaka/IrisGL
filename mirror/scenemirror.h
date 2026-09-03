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
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QMatrix4x4>
#include <QSet>
#include <utility>
#include <vector>
#include "irisgl/irisglfwd.h"
#include "irisgl/document/animation/clipextractor.h"
#include "jahshaka/engine/Engine.h"

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
    static void skinVertices(const QVector<QMatrix4x4> &boneTransforms,
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
    bool boneWorldTransforms(QHash<QString, QMatrix4x4> &out) const;
    /// Pushes a world matrix onto an engine node as TRS (used by overlays too).
    static void pushTransform(jahshaka::engine::Scene *scene, jahshaka::engine::NodeId node, const QMatrix4x4 &world);
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
    static QImage bakeRealisticSky(const iris::SkyRealistic &sky, int width, int height);

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
        bool hasMesh  = false;
        bool hasLight = false;
        jahshaka::engine::MaterialId material = 0;   // per document material instance
        iris::Material *materialPtr = nullptr;
        jahshaka::engine::MeshId mesh = 0;           // shared engine mesh this entry uses
        iris::Mesh *meshPtr = nullptr;
        QString textureSignature;                    // which files are bound; re-sync on change
        jahshaka::engine::NodeId wireNode = 0;       // light wire shape, child of `node`
        jahshaka::engine::MaterialId wireMaterial = 0;
        int wireKind = -1;                           // which shape is attached
        bool hasIcon = false;                        // light icon billboard on wireNode
        QString iconSignature;                       // icon image path; recreate on change
        bool hasBillboards = false;                  // particle emitter mirrored as billboards
        QString billboardSignature;                  // texture + blend; recreate on change
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
    void syncParticles(Entry &e, iris::ParticleSystemNode *ps);
    void syncLightWires(Entry &e, iris::LightNode *light);
    void syncLightIcon(Entry &e, iris::LightNode *light);
    jahshaka::engine::TextureId iconTextureFor(const QString &path);
    void syncHighlight();
    void syncGrid();
    jahshaka::engine::MeshId wireMeshFor(int kind);
    void visit(iris::SceneNodePtr node, jahshaka::engine::NodeId parent, QSet<long> &seen);
    void removeMissing(const QSet<long> &seen);
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
    static jahshaka::engine::LightDesc toLightDesc(iris::LightNode *light);
private:

    jahshaka::engine::Scene *mTarget;
    iris::ScenePtr           mSource;
    QHash<long, Entry>       mEntries;         // keyed by iris SceneNode::nodeId
    QHash<iris::Mesh *, jahshaka::engine::MeshId> mMeshes;
    /// Reused across frames so a per-frame pose push allocates nothing.
    std::vector<jahshaka::engine::BonePose> mPoseScratch;
    QHash<iris::Material *, jahshaka::engine::MaterialId> mMaterials;
    QHash<QString, jahshaka::engine::TextureId> mTextures;
    QHash<QString, jahshaka::engine::TextureId> mIconTextures;   // light icon glyphs (Qt resources)
    jahshaka::engine::MaterialId mDefaultMaterial = 0;
    bool mLightWires = true;
    QString mSkySignature;
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
    jahshaka::engine::MeshId mWireMeshes[4] = { 0, 0, 0, 0 };   // directional, point, spot, area
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
    // Largest shadow-map resolution any shadow-casting light asked for, from the
    // last sync(); pushed engine-wide by applyEnvironment (the engine's atlas is
    // global, like the filter — rebuild is expensive, so only on change).
    unsigned mMaxShadowResolution = 0;
    // Global illumination: last pushed state + the driving light's transform, so
    // applyEnvironment only re-pushes on change and re-traces on light movement.
    jahshaka::engine::GiParams mLastGi;
    bool mGiPushed = false;
    QMatrix4x4 mGiLightWorld;
    // Fog: last pushed state. Enabling/disabling fog creates or destroys the
    // scene's atmosphere and changes the shader variant, so this one is pushed on
    // change only, not every frame.
    jahshaka::engine::FogDesc mLastFog;
    bool mFogPushed = false;
};

#endif // SCENEMIRROR_H
