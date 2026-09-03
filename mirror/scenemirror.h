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
    /// The live pose as engine bone poses, index-parallel to toSkeletonDesc's
    /// bones: each bone's transform LOCAL to its parent, decomposed to TRS from
    /// the document's skin matrices. False when the skeleton is null/empty or its
    /// boneTransforms are the wrong size.
    static bool toBonePoses(const iris::SkeletonPtr &skeleton,
                            std::vector<jahshaka::engine::BonePose> &out);
    /// CPU-skins bind-pose vertices with the skeleton's live boneTransforms —
    /// exactly the legacy GL shader's math (weighted sum of bone matrices).
    /// bindNormals/outNormals may be empty. Static and public for tests.
    ///
    /// NOT PART OF RENDERING any more (GPU_SKINNING_SPEC §6): skinning happens in
    /// the vertex shader, and sync() never calls this. It survives as the ORACLE
    /// the GPU path is checked against (skeletal.gpu_parity) and for document-only
    /// contexts that need posed vertices with no GPU. Note it skins position and
    /// normal only — the shader also skins the TANGENT, so a normal-mapped
    /// character lights correctly on the GPU path and did not on this one.
    static void skinVertices(const QVector<QMatrix4x4> &boneTransforms,
                             const std::vector<float> &bindPositions,
                             const std::vector<float> &bindNormals,
                             const std::vector<float> &boneIndices,
                             const std::vector<float> &boneWeights,
                             std::vector<float> &outPositions,
                             std::vector<float> &outNormals);
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

    /// Cosine-weighted hemisphere averages of an equirect sky image, in LINEAR
    /// light (the image bytes are sRGB): what the scene's ambient becomes when
    /// `Scene::ambientFromSky` is on (VISUAL_PARITY_SPEC item 3b). Row 0 of the
    /// image is the zenith, matching the engine's sky sphere mapping. Returns
    /// false for a null image. Public for tests.
    static bool integrateSkyAmbient(const QImage &equirect,
                                    jahshaka::engine::Colour &upperOut,
                                    jahshaka::engine::Colour &lowerOut);

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
        QVector<QMatrix4x4> lastPose;                // last pose pushed for THIS node
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
    /// Per-frame skinning, all of it (GPU_SKINNING_SPEC phase 3): for every
    /// GPU-skinned NODE whose pose changed since the last push, decompose the
    /// document's skin matrices to per-bone local TRS and push them with
    /// Scene::setBonePoses. O(bones), not O(vertices); nothing is uploaded.
    void syncBonePoses();
    /// Resamples an equirect sky image into six small cubemap faces and pushes
    /// them as the scene's environment reflections (Scene::setSkyReflection) —
    /// how equirect/gradient/realistic skies get the IBL cubemap skies have.
    /// Also records the sky's ambient integral for applyEnvironment (item 3b).
    void applySkyReflection(const QImage &equirect);
    /// Cubemap skies do not go through applySkyReflection (the engine takes the
    /// six faces directly), so their ambient integral is taken from the face
    /// images: the same cosine-weighted upper/lower split, per face texel.
    void recordCubeAmbient(const QImage faces[6]);
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
    jahshaka::engine::Colour mLastAmbientUpper, mLastAmbientLower;
    jahshaka::engine::Colour mSkyAmbientUpper { 0.0f, 0.0f, 0.0f, 1.0f };
    jahshaka::engine::Colour mSkyAmbientLower { 0.0f, 0.0f, 0.0f, 1.0f };
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
};

#endif // SCENEMIRROR_H
