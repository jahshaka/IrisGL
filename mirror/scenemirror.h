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
#include <QList>
#include <QImage>
#include <QSet>
#include <utility>
#include <set>
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
struct NodeOpaque;
using NodeHandle = NodeOpaque *;
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

    /// The shadow-refresh serial this mirror last acted on
    /// (world.refreshShadows(); the giRefreshSerial shape).
    quint64 mShadowRefreshSerialSeen = 0;

    /// The engine MATERIAL the mirror created for a document node, or 0.
    /// DIAGNOSTIC use (Scene::dumpMaterial): the answer to "what did the
    /// backend datablock actually end up holding" needs the id the mirror
    /// pushed to, and nothing outside the mirror knows it.
    jahshaka::engine::MaterialId engineMaterial(const iris::SceneNode *node) const;

    /// Points `view`'s camera where the document camera is looking.
    ///
    /// `framingAspect` is the WIDE-ASPECT FRAMING HOLD (owner report
    /// 2026-09-07, re-scoped 2026-09-08;
    /// jahshaka::engine::CameraDesc::framingAspect): zero — the default —
    /// leaves the authored vertical angle alone, which is what every AUTHORED
    /// camera gets. Only a host that knows it is pushing a FREE camera (the
    /// editor explorer, the player's fly camera) passes one, and only ABOVE
    /// that aspect does the vertical angle narrow — to hold the horizontal
    /// extent the shot has there — instead of letting an ultra-wide window
    /// fisheye it. At or below it the shot is untouched, bit for bit.
    void applyCamera(iris::CameraNodePtr camera, jahshaka::engine::View *view,
                     float framingAspect = 0.0f);
    /// Points `view`'s picture-in-picture inset at a document camera
    /// (CAMERAS_SPEC D3, phase 2c). `desc` carries everything but the camera —
    /// the rect, the inset's background, the offscreen opt-in — and this fills
    /// in `desc.camera` from the document node, the same translation
    /// applyCamera does for the main view.
    ///
    /// It also fills in the inset's GRADE — whether the shot is tonemapped and
    /// at what exposure — by laying the PIPPED camera's own exposure and post
    /// overrides over the world's description (CAMERA_LENS_SPEC §4/§5's
    /// substitution, over the world value applyEnvironment recorded). So a
    /// camera's own look appears in its preview and NOWHERE ELSE: nothing here
    /// touches the view's post description. Call applyEnvironment first (hosts
    /// already do, every frame) so the world's half is current.
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
    ///
    /// GI is the exception (ENGINE_CACHE_POLICY_SPEC P10): it is NOT re-pushed —
    /// a GI push is a from-scratch rebuild — the next applyEnvironment calls
    /// Scene::reassertGiBinding instead, which re-points the binding at this
    /// scene's still-valid arms and rebuilds nothing.
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
    /// How many times the CHEAP light-only re-inject ran instead of a full
    /// re-solve (REFLECTIONS_ADOPTION_SPEC.md P2). During a light drag this is
    /// the counter that moves; giRefreshCount() stays still until the drag ends.
    quint64 giLightRefreshCount() const { return mGiLightRefreshCount; }
    /// ...of which the ones taken AT REST (round-2 review F1): the injection is
    /// deliberately cheap while something moves — one bounce, the coarse ray
    /// march — so the tick that ENDS the motion must run the scene's full count
    /// instead, and for a MOVABLE lamp this mirror is the only thing that knows
    /// when that is (the path arms no settle). One per burst of movement.
    quint64 giLightRefreshAtRestCount() const { return mGiLightRefreshAtRestCount; }

    /// HOW MANY MATERIAL DESCRIPTIONS THE LAST SYNC BUILT (MIRROR_SCALE lane).
    /// Building one converts the document material into a PbrParams and a
    /// texture-bind list — a dozen heap allocations, two dynamic_casts and a
    /// pass over the slot table — and it used to happen once per distinct
    /// material per FRAME, which in this editor means once per mesh node per
    /// frame (every primitive is born with its own PbrMaterial). It is now
    /// keyed on a fingerprint of the material's own fields, so a STILL frame
    /// must build ZERO. Not observable in pixels; hence the counter, and hence
    /// mirror.scale's assertion on it.
    quint64 materialBuildCount() const { return mMaterialBuilds; }
    /// How many document nodes the last sync walked (sync()'s own return, kept
    /// so a reader that did not call it can still ask). Since the dirty set
    /// (DIRTY_SET_MIRROR_SPEC) this is the MARKED set, not the scene: on a
    /// still frame it is ZERO however big the document is.
    int visitedCount() const { return mVisited; }
    /// HOW MANY DOCUMENT NODES THIS MIRROR CURRENTLY HOLDS AN ENTRY FOR. The
    /// question `sync()`'s return value used to answer by accident, back when
    /// the walk reached every node every frame; since the dirty set it is the
    /// only honest spelling of "is this node still mirrored?".
    quint64 mirroredNodeCount() const { return quint64(mEntries.size()); }
    /// Every document node this mirror holds an entry for, by name. Diagnostic
    /// (the mirror suites print it when a count assertion fails).
    QStringList mirroredNodeNames() const;
    /// The EFFECTIVE VISIBILITY this mirror last pushed for `node` (1 shown,
    /// 0 hidden, -1 never pushed / not mirrored). The engine has no read-back
    /// for it, and it is the contract the F6 case asserts: since ENGINE-3 the
    /// mirror is the SOLE pusher of a document node's effective visibility.
    int pushedVisibility(const iris::SceneNode *node) const;

    // ---- THE DIRTY SET (SPECS/DIRTY_SET_MIRROR_SPEC.md, owner option A) ----
    //
    // The mirror handles what CHANGED instead of asking every object in the
    // document once a frame. The counters below are the contract, and
    // editor.mirrorStats() reports every one of them.

    /// How many nodes the document handed over on the last sync — the size of
    /// the change list, before the visit. ZERO on a still frame.
    quint64 dirtyNodeCount() const { return mDirtyNodes; }
    /// How many entries the last sync released because their nodes left the
    /// document (the eviction list that replaced the stamp sweep).
    quint64 evictedNodeCount() const { return mEvictedNodes; }
    /// How many nodes the amortised VERIFIER re-checked on the last sync — the
    /// rotating slow re-read that catches a write which bypassed the funnel.
    quint64 verifierVisitCount() const { return mVerifierVisits; }
    /// How many times the verifier has FOUND one: a latch it had to push, or a
    /// material whose fields moved without its revision. Cumulative, and it
    /// must be zero — every catch is a missing mark, named once in the log.
    quint64 verifierCatchCount() const { return mVerifierCatches; }
    /// How many engine pushes the node visits have made, ever. The oracle's
    /// instrument: a full verification walk straight after a dirty sync must
    /// not move it (see verifyAgainstFullWalk).
    quint64 visitPushCount() const { return mVisitPushes; }
    /// "dirty" or "full" — which mode the LAST sync ran in. A full walk is the
    /// explicit, rare answer (a bind, a graph re-take, the play edge, an
    /// eviction overflow, JAH_MIRROR_VERIFY=full).
    const char *walkMode() const { return mLastWalkWasFull ? "full" : "dirty"; }

    /// Forces the NEXT sync to walk the whole document. The hosts' explicit
    /// "everything may have changed" (a bind, a re-take, a play edge); tests
    /// use it to compare the two modes.
    void requestFullWalk() { mFullWalkPending = true; }

    /// THE ORACLE, and the one reason the full walk still exists: runs the
    /// complete walk over the document WITHOUT consuming the change list, and
    /// answers how many engine pushes it made. After a correct dirty sync that
    /// is ZERO — anything else is a mark the document did not raise. A material
    /// is re-FINGERPRINTED during this walk rather than trusted to its
    /// revision, so a field written behind Material::touch()'s back is caught
    /// too. `mirror.dirty_equals_full` drives it after every mutation class.
    quint64 verifyAgainstFullWalk();

    /// How many entries the amortised verifier re-checks per sync (default 64:
    /// a full pass over an 8,404-node scene every ~2.2 s at 60 Hz, for about a
    /// tenth of a millisecond a frame). 0 turns it off.
    void setVerifierBudget(unsigned perSync) { mVerifierBudget = perSync; }
    unsigned verifierBudget() const { return mVerifierBudget; }
    /// ...and how many MATERIALS it re-fingerprints per sync (default 8).
    void setVerifierMaterialBudget(unsigned perSync) { mVerifierMaterialBudget = perSync; }
    unsigned verifierMaterialBudget() const { return mVerifierMaterialBudget; }
    /// EVERY sync runs the whole verification walk. What `JAH_MIRROR_VERIFY=full`
    /// sets and what the differential suite runs under; ruinous for frame time
    /// and exact by construction.
    void setVerifyEverything(bool on) { mVerifyEverything = on; }
    bool verifyEverything() const { return mVerifyEverything; }

    /// ---- SCENE_STATIC, the settle half (MIRROR_SCALE lane) ---------------
    /// How many nodes are in a SCENE_STATIC memory manager right now
    /// (iris::graph::staticNodeCount), and how many times this mirror has
    /// re-derived the whole scene's classification after the document went
    /// quiet. A user nudging props used to drain the first number to zero for
    /// the session; the second is what puts it back. Both are on
    /// editor.mirrorStats() so a test — and a lead reading a live app — can see
    /// a scene's classification hold instead of leaking away.
    quint64 staticNodeCount() const;
    quint64 staticRepromotionCount() const { return mStaticRepromotions; }
    /// How many consecutive syncs with NO transform write anywhere in the
    /// document count as "settled". Half a second at 60 Hz: long enough that a
    /// drag's inter-frame gaps never trip it, short enough that the
    /// classification is back before the user's next gesture.
    static constexpr quint32 kStaticSettleFrames = 30;
    /// A NOTE ON THE PAUSED HAND (lead review F4): the settle is driven by the
    /// document, not by the mouse, so a drag the user pauses for half a second
    /// re-promotes under a still hand and the next movement demotes again. That
    /// is correct (the classification always describes what the scene is doing)
    /// and it is bounded — one pass per pause — but it is worth knowing before
    /// reading a staticRepromotions count taken during a slow edit.

    // ---- MOBILITY (REALTIME_REFLECTIONS_SPEC §3.3, lane R1) ----------------
    /// How many of the document's nodes resolved MOVABLE on the last sync —
    /// what the mirror pushed to the engine, which is also what the engine
    /// reports back through Scene::mobilityStatus. Zero for a still scene of
    /// props, which is the whole point of the classification.
    quint64 movableNodeCount() const { return mMovableNodes; }
    /// SOFT PROMOTIONS (§3.3.3, O3): nodes that started moving during play
    /// with nothing predicting they would. Each one is a "mark it Movable"
    /// message for the author, counted once per node per play session, and each
    /// leaves a stale bounce-light ghost where it started until play stops.
    quint64 mobilityMissCount() const { return mMobilityMisses; }
    /// The name of the last one, for the message a host shows.
    QString lastMobilityMiss() const { return mLastMobilityMiss; }

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

    // ---- The CHARACTER rig (AVATAR_RIG_PERF_SPEC §3.1, decision D5 = U2) ----
    //
    // An imported character is several skinned pieces (body, head, eyes, hair)
    // and `Mesh::extractSkeleton` gives each piece a rig of ITS OWN BONES, in
    // ITS OWN ORDER — five pieces, five rigs, five SkeletonDefs, five
    // SkeletonInstances. Ogre refuses to let two Items share a SkeletonInstance
    // unless their meshes name the SAME skeleton (OgreItem.cpp:249-254), so
    // sharing is not merely unhelpful on per-piece rigs, it is illegal. The
    // prerequisite is ONE rig per CHARACTER — the union of the pieces' bones in
    // a canonical order — plus a per-piece blend-index remap, which Ogre already
    // has a slot for (SubMesh::mBlendIndexToBoneIndexMap) and which pays for
    // itself even without sharing: HlmsPbs streams the MAP per draw, so a
    // compacted map streams the piece's own bones instead of the whole rig.
    //
    // MIRROR-SIDE, not document-side (D5's U2): no file format changes, the
    // pieces keep their own skeletons and their own blend indices, and the
    // union is derived — so a re-import or a document edit cannot leave a stale
    // character rig on disk.

    /// The UNION of several pieces' rigs, as one iris::Skeleton.
    ///
    /// Bones are merged BY NAME. A bone's bind pose comes from the first piece
    /// that carries it, and a piece that disagrees about a shared bone's bind
    /// pose is EXCLUDED from the union (its index is appended to `excluded`)
    /// rather than silently averaged — it keeps its own rig and its own
    /// instance, which is slower and correct.
    ///
    /// The hierarchy is rebuilt from what the pieces together know: a piece
    /// records the nearest ancestor IT carries, so the union takes, for each
    /// bone, the DEEPEST of the ancestors any piece named. Bone ORDER is
    /// canonical (depth, then name) precisely so the union does not depend on
    /// which piece was visited first — the rig id is a structure hash, and an
    /// order that depended on visit order would make the same character hash
    /// two different rigs on two different loads.
    ///
    /// False when fewer than two pieces merged, when the pieces' hierarchies
    /// disagree, or when the result would be cyclic. A single-piece character
    /// therefore keeps its own rig BYTE FOR BYTE — the negative gate.
    static bool buildUnionSkeleton(const QVector<iris::SkeletonPtr> &pieces,
                                   iris::SkeletonPtr &out,
                                   QVector<int> *excluded = nullptr,
                                   QString *why = nullptr);
    /// `out[i]` = the index in `rig` of the piece's bone `i` — the blend-index
    /// map `attachSkinnedMesh` takes. False when the rig does not carry one of
    /// the piece's bones (which is what the union guarantees it does).
    static bool rigRemap(const iris::SkeletonPtr &piece, const iris::SkeletonPtr &rig,
                         QVector<unsigned short> &out);
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
    /// How many socket attachments did NOT resolve on the last sync — a stale
    /// owner, a removed socket, a bone a re-import renamed. Diagnostic only:
    /// a dangling attachment leaves its rider exactly where it was.
    int lastSocketDangling() const { return mSocketDangling; }
    /// The socket resolver, for tests and for hosts that want the stale
    /// count. Its pose source is installed by this mirror's constructor.
    iris::SocketResolver &socketResolver() { return mSockets; }
    /// How many `setClipStates` calls this mirror has made since it was built
    /// (AVATAR_RIG_PERF_SPEC §1 row 2, §3.5).
    ///
    /// The per-frame clip push is ONE engine call per skinned node, so a
    /// character made of five skinned pieces costs five — which is the cost row
    /// the rig-perf program removes by pushing once per CHARACTER. A counter
    /// rather than a log line because the claim is a NUMBER: the bench records
    /// it, and the gate asserts pushes-per-frame == characters.
    quint64 clipStatePushes() const { return mClipStatePushes; }
    /// Pushes a world matrix onto an engine node as TRS (used by overlays too).
    static void pushTransform(jahshaka::engine::Scene *scene, jahshaka::engine::NodeId node, const iris::Mat4 &world);
    /// The engine mesh already created for a document mesh, or 0.
    jahshaka::engine::MeshId engineMesh(iris::Mesh *mesh) const;

    /// The whole selected SET (EDITOR_MULTISELECT_SPEC §2.3). The shell walk
    /// was always N-mesh — one shell per mesh under the highlighted node — so
    /// N ROOTS is the same walk started N times; the pooling, the reclaim and
    /// the skinned-silhouette handling are untouched.
    ///
    /// `primary` is the set's PRIMARY member (D4 b, the Blender rule): its
    /// shells carry `scene->outlinePrimaryColor` — brighter — while the rest
    /// carry `scene->outlineColor`. It is passed EXPLICITLY rather than read
    /// off the front of `nodes` because the viewport filters the set before it
    /// gets here (the World root and the built-in ground never outline), so
    /// "first in the list" and "the primary" are not the same node.
    ///
    /// The two colours are used only when the set has MORE THAN ONE member: a
    /// single selection has nothing to distinguish and stays pixel-identical to
    /// what it was before the primary colour existed (app.selection_outline's
    /// gate is written against exactly that).
    ///
    /// The single-node overload this replaced (`setHighlightedNode`) had no
    /// callers left after the multi-select program and is deleted rather than
    /// kept as a second way to say the same thing.
    void setHighlightedNodes(const QList<iris::SceneNodePtr> &nodes,
                             const iris::SceneNodePtr &primary = iris::SceneNodePtr());
    /// Whether this node is IN the highlighted set (the light/camera wires ask,
    /// and equality against one node stopped being the right question).
    bool isHighlighted(const iris::SceneNode *node) const;

    /// Selection highlight look: false (default) = silhouette outline (inverted
    /// hull); true = the on-top polygon wireframe.
    void setHighlightWireframe(bool on);
    bool highlightWireframe() const { return mHighlightWireframe; }
    /// Pins the shader clock generated pieces read (HLMS_ADOPTION P5) to an
    /// exact number of seconds; a NEGATIVE value hands it back to the wall
    /// clock. This is what makes an animated graph material reproducible —
    /// a suite that renders at t = 0.25 gets the same pixels every run, and a
    /// scrubbed timeline can drive the surface from its own playhead.
    void setShaderTimeOverride(float seconds) { mShaderTimeOverride = seconds; }
    float shaderTimeOverride() const { return mShaderTimeOverride; }
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
    /// Which world plane the grid lies in. Floor is the classic XZ ground
    /// grid; FrontXY / SideYZ exist for the canonical orthographic views,
    /// where a floor grid seen edge-on is a single useless line — the
    /// viewport picks the plane that faces the view axis.
    enum class GridPlane { Floor, FrontXY, SideYZ };
    void setGrid(bool visible, float spacing, GridPlane plane = GridPlane::Floor);
    bool gridVisible() const { return mGridVisible; }

    /// How far the grid reaches from the origin, in world units (default 100 =
    /// the editor's ±100 floor). A preview whose subject is a 170-unit-tall
    /// character needs a bigger one, or the "floor" is smaller than the thing
    /// standing on it. Changing it rebuilds the grid meshes on the next sync.
    void setGridExtent(float extent);
    /// HOW FAR ABOVE ITS OWN PLANE THE FLOOR GRID IS DRAWN, in world units
    /// (GIZMO-2 item 5). The grid is a helper drawn WITH the depth test, so
    /// geometry in front of it covers it; this lift is what keeps it from
    /// z-fighting with a ground plane at the same height. Measured at the knee
    /// of the curve — see syncGrid.
    static constexpr float kGridFloorLift = 0.01f;
    /// Grid line colours (minor, major). Alpha is the line's opacity. The
    /// editor keeps its blue-grey default; the avatar preview asks for white,
    /// and the canonical axis views ask for a per-plane tint.
    void setGridColours(const jahshaka::engine::Colour &minor,
                        const jahshaka::engine::Colour &major);

    /// THE GI VOLUME OVERLAY (LIGHTING_FIX fix 9). Draws the two boxes
    /// `Scene::giStatus()` reports — the lit (voxel) volume and, in the hybrid,
    /// the reflection-probe region — as editor-helper wireframes.
    ///
    /// It exists because the lit volume is the single most consequential thing
    /// in a GI scene that a user cannot see: an object outside it gets no
    /// bounce and no VCT ambient, and the only symptom is "that corner looks
    /// wrong". Off by default; `editor.setOverlays({giVolume: true})` is the
    /// verb, and the boxes carry kHelperBit so they can never appear in a probe
    /// capture or a shadow map. Draws nothing at all while GI is off.
    void setGiVolumeOverlay(bool visible);
    bool giVolumeOverlay() const { return mGiVolumeVisible; }

private:
    /// Records which camera is driving `view` and answers "did it CHANGE" — the
    /// cut test the exposure re-seed rides on (CAMERA_LENS_SPEC §4). False the
    /// first time a view is seen: an opening frame is not a cut.
    bool noteDrivingCamera(const jahshaka::engine::View *view, const iris::CameraNodePtr &camera);
    /// The camera last recorded as driving `view`, or null (never seen, or the
    /// node has since been deleted). Read by applyEnvironment, which builds the
    /// view's post description a beat BEFORE applyCamera runs.
    iris::CameraNodePtr drivingCameraFor(const jahshaka::engine::View *view) const;

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
        /// The EFFECTIVE visibility last pushed (the node's own flag AND every
        /// ancestor's — SceneNode::isVisibleInScene); -1 = never. Pushed on
        /// CHANGE only, and a change of an ANCESTOR is a change here.
        int visiblePushed = -1;
        /// The `pickable` flag last pushed onto this node's engine objects as
        /// Ogre QUERY FLAGS; -1 = never. Ogre's RaySceneQuery is the picking
        /// broad phase now (SCENEGRAPH_SPEC §2), and its mask is tested inside
        /// the SIMD sweep — so unpickable geometry has to carry the bit that
        /// keeps it out. Change-guarded like visibility, and re-pushed whenever
        /// geometry is (re-)attached, because the flags live on the Item and a
        /// new Item is born with the default mask.
        int pickablePushed = -1;
        /// The `mMaterialItemSerial` this entry last pushed query flags
        /// against. Behind = the material's Items were rebuilt (a shading-model
        /// switch) and the new ones carry the default mask.
        quint32 materialItemSerial = 0;
        /// The LIGHTING CHANNEL mask last pushed onto this node's engine
        /// objects, and whether one ever was. Unlike the query flags above this
        /// does NOT need re-pushing when geometry is re-attached: the engine
        /// keeps the mask on its own node record and re-applies it to every
        /// Item it builds (OgreScene::setNodeLightMask's contract), precisely
        /// so a material swap cannot silently un-mask an object.
        quint32 lightMaskPushed = 0xFFFFFFFFu;
        bool    lightMaskEverPushed = false;
        /// PER-OBJECT SHADOW CASTING (SceneNode::castShadow). Change-guarded
        /// like the light mask; -1 = never pushed.
        int     castShadowPushed = -1;
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
        /// (The per-ENTRY copy of the last-pushed PbrParams is GONE. It was
        /// redundant by construction — `material` is the ENGINE material id,
        /// shared by every entry using the same document material, and the
        /// params come from the per-material memo — so N entries each kept
        /// their own copy and each ran a full compare per frame to let one of
        /// them push. Harmless while PbrParams was small; the workflow, detail
        /// and sampler rows grew it ~2.2x and a.sync_dynamic@50000 doubled
        /// (83.7 -> 182.2 ms, measured against the base build; the idle paths
        /// did not move, which is what localised it). The guard now lives in
        /// mPbrPushed, keyed by the engine material id: one copy and one
        /// compare per MATERIAL per sync instead of per NODE.)
        /// The shading model last ATTEMPTED for `material` (-1 = never). Its own
        /// field because the switch is its own engine verb: the two shading
        /// families are different backend material types, so a switch destroys
        /// the backend material, rebuilds it and re-attaches every renderable —
        /// not something a per-frame parameter push may do.
        ///
        /// "ATTEMPTED", not "pushed", and that is deliberate — the same shape as
        /// `planarReflector` above. The engine REFUSES Unlit on a material a
        /// rigged mesh uses (the family cannot skin), and a refusal the mirror
        /// forgot would be retried, and re-reported, sixty times a second.
        int shadingModelPushed = -1;
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
        /// The wire node's local scale, as a signature, and the visibility last
        /// pushed to it (-1 = never). Both were written every frame for every
        /// light in the scene; both are derived from hand-edited values (audit
        /// F7).
        quint64 wireXformKey = 0;
        bool wireXformPushed = false;
        int wireVisible = -1;
        bool hasIcon = false;                        // light icon billboard on wireNode
        QString iconSignature;                       // icon image path; recreate on change
        /// The icon billboard's instance (world position + size) as a
        /// signature. setBillboards rewrites the set's whole instance buffer,
        /// and a still light's icon does not move.
        quint64 iconKey = 0;
        bool iconPushed = false;
        /// The LightDesc last pushed, and whether one ever was. setLight is
        /// ~20 Ogre setters including an attenuation solve and an AABB rewrite;
        /// the mirror still LOOKS every frame (the panel can edit any value)
        /// but pushes only a change — exactly the discipline `lastPbr` gives
        /// materials (audit F7).
        jahshaka::engine::LightDesc lastLight;
        bool lightPushed = false;
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
        // GI bounds exclusion (REFLECTIONS_ADOPTION_SPEC.md P1a.2), same
        // push-on-change discipline: the engine invalidates its GI caches when
        // the flag really changes, so re-pushing it every frame would flag a
        // rebuild every frame. -1 = never pushed.
        int giBoundsExcluded = -1;
        // MOBILITY (REALTIME_REFLECTIONS_SPEC §3.3, lanes R1/R2): the last
        // RESOLVED answer pushed to the engine, same push-on-change discipline.
        // -1 = never pushed. The discipline is load-bearing now that the engine
        // spends it: a flip is a classification change, and crossing the GI
        // edge costs a from-scratch invalidation or a probe-grid stale.
        int movable = -1;
        /// Whether the movable state above was a PLAY-TIME SOFT promotion. It
        /// decides the intent of the push that CLEARS it too: a soft promotion
        /// never took the object out of the voxel bounce (that is what makes it
        /// free), so putting it back must not pay a from-scratch rebuild
        /// either.
        bool movableSoft = false;
        // SOFT PROMOTION (§3.3.3, owner decision O3). The pose this node was
        // last seen at while playing, and whether the author has already been
        // told about it. `posed` is false until the first play frame sees it —
        // a node has to be seen standing still before it can be seen moving.
        //
        // THESE TWO LATCHES ARE PER MIRROR, the flag they set is per DOCUMENT
        // (SceneNode::_setSoftMovable). One scene drawn by TWO mirrors — the
        // editor and the player viewport both syncing the same document, which
        // nothing does today because the visible page owns the graph (see
        // sync()'s re-take) — would therefore warn about the same object twice,
        // once per mirror, while the promotion itself stays correct (the flag
        // is idempotent and the document clears it). If a second simultaneous
        // mirror ever becomes real, the warn latch belongs on the node beside
        // the flag, not here.
        iris::Vec3 playPos, playScale;
        iris::Quat playRot;
        bool posed = false;
        bool mobilityWarned = false;
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
        /// The skeleton the ENGINE rig was actually built from: this piece's own
        /// `skeleton` for a lone piece, the CHARACTER UNION for a piece of a
        /// multi-piece character (AVATAR_RIG_PERF_SPEC §3.1). Everything that
        /// speaks the engine's bone indices — the clip extraction, the bone
        /// read-back, the socket FK — reads THIS, not `skeleton`, because those
        /// indices are the union's.
        iris::SkeletonPtr rigSkeleton;
        /// This piece's blend index -> union bone index map, empty for a piece
        /// whose rig is its own (the identity).
        QVector<unsigned short> blendToRig;
        /// The character this piece belongs to and the union epoch it attached
        /// with. A character whose piece set changes (a re-import adding a
        /// piece with new bones) gets a NEW union, and every piece already
        /// attached to the old one has to re-attach — the epoch is how a piece
        /// notices without re-deriving the union every frame.
        const iris::SceneNode *characterHost = nullptr;
        quint32 characterEpoch = 0;
        /// The engine node whose SkeletonInstance this piece is rendering from,
        /// or 0 when it owns its own (AVATAR_RIG_PERF_SPEC §3.4). A follower is
        /// skipped by the clip pass — it has no animation state of its own — so
        /// this is what turns five clip pushes per character into one.
        jahshaka::engine::NodeId shareMaster = 0;
        /// SKELETON SHARING's eligibility answer, memoised: whether this piece
        /// may render from its master's SkeletonInstance. It is a rig-id match
        /// plus a world-transform comparison, and the transform half needs
        /// `getGlobalTransform()` on the piece AND the master — per piece, per
        /// frame, to re-derive an answer that can only change when something
        /// writes a transform. Invalidated by the document's write counter and
        /// by a re-attach (which can change `rigId`).
        bool shareEligible = false;
        bool shareEligibleValid = false;
        bool gpuSkinned = false;                     // the engine accepted the rig
        size_t boneCount = 0;
        /// Each bone's PARENT INDEX, resolved once per rig instead of by a
        /// QHash<QString> probe on the parent bone's NAME per bone per frame
        /// (audit F13 — that walk became per-frame work the moment a scene had
        /// a socket). PER ENTRY, not one shared slot: a scene with two rigged
        /// characters would thrash a single cache back to the string lookups it
        /// replaces. `boneParentsOwner` is only ever compared, and it cannot
        /// dangle — `skeleton` above is a strong reference to it.
        mutable std::vector<int> boneParents;
        mutable const iris::Skeleton *boneParentsOwner = nullptr;
        // Clip playback (ANIMATION_ENGINE_MIGRATION_SPEC M3). The document says
        // WHICH clip and WHEN; the engine samples and blends it.
        iris::SceneNode *docNode = nullptr;          // the document node this entry mirrors
        // ---- WHAT THIS ENTRY CONTRIBUTES TO THE SCENE-WIDE AGGREGATES -----
        // §1.4's folds used to be recomputed by the walk every frame. They are
        // kept incrementally instead: an entry adjusts the counter on its own
        // TRANSITION and gives its contribution back when it is released, so a
        // still frame recomputes nothing and a dirty visit costs one compare.
        bool countedRefractive = false;
        bool countedDistortion = false;
        bool countedSkinned = false;
        bool countedPiece = false;
        bool countedMovable = false;
        /// The DecalDesc last pushed, as a hash. Decals were the one unlatched
        /// per-frame push left in the walk (DIRTY_SET_MIRROR_SPEC §4).
        quint64 decalPushKey = 0;
        std::string rigId;                           // for the clip def's content key
        /// The rig + clip set, as a HASH. It was a QString built by
        /// concatenation — a Mixamo character with 30 clips cost ~100
        /// allocations per frame to produce a string whose only use was a
        /// compare against last frame's (MIRROR_SCALE lane). 0 = "no clips
        /// attached", which is what the re-attach sites write.
        quint64 clipSignature = 0;
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
        /// Document clip NAME -> the name the engine gave it — the third map,
        /// and the only one the LOCOMOTION push can use (AVATAR_LOCOMOTION_SPEC
        /// Stage 5). The state machine names its clips the way a human does,
        /// because a scene file and a role binding both store a name; it has no
        /// AnimationPtr to hand over. Built alongside the other two so the two
        /// paths cannot drift.
        ///
        /// FIRST WINS on a duplicate name, which is not a shrug: the document
        /// side collapses duplicates the same way (`collectAvatarClips` skips a
        /// name it already has), so both ends pick the same clip out of a
        /// character carrying two "mixamo.com"s. The AnimationPtr-keyed map
        /// above stays the authored path's lookup precisely because THAT path
        /// can tell the two apart and this one cannot.
        QHash<QString, QString> clipNameMap;                  // document clip name -> engine clip name
        /// One clip state as last pushed to the engine — the change-detection
        /// latch, generalised from Stage 4's single {name, time, looping}
        /// triple to the N weighted states a blend space publishes.
        ///
        /// `name` is the ENGINE's clip name and it is deliberately the very
        /// QString stored in `clipIdMap` / `clipNameMap`: Qt strings are
        /// implicitly shared, so comparing it against next frame's lookup is a
        /// d-pointer compare and not a strcmp, and NOTHING is converted to
        /// std::string on a frame whose push is skipped.
        struct ClipPush {
            QString name;
            float   time = 0.0f;
            float   weight = 0.0f;
            bool    looping = false;
        };
        /// Empty means "nothing is enabled on this node right now", which is
        /// also what the disable push leaves behind.
        QVector<ClipPush> lastClipPush;
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
    /// Resolves `focusMode == Track` into `focusDistance` for one camera, in
    /// world space, after sockets have posed everything (CAMERA_LENS_SPEC §3
    /// P2). No-op for Manual and Off — and for a target guid that no longer
    /// resolves, which freezes the last distance rather than snapping.
    void resolveFocusTracking(iris::CameraNode *camera);
    jahshaka::engine::TextureId iconTextureFor(const QString &path);
    void syncHighlight();
    void syncGrid();
    /// THE GROUND'S HORIZON (owner, 2026-09-13: "should the default ground not
    /// also be infinite in the Grand Showroom 2? It seems cut off"). A single
    /// mirror-owned plane, far beyond anything a user flies to, drawn under the
    /// scene's DEFAULT FLOOR in the floor's own material so the checker simply
    /// carries on to the horizon instead of ending in a square edge. It is an
    /// EDITOR HELPER in the engine's sense (kHelperBit): drawn by the view and
    /// by nothing else — no GI geometry, no voxel bounce, no reflection-probe
    /// capture, no shadow map, no probe staleness — so it changes no cache at
    /// all. IT IS A SEPARATE ITEM on the floor's datablock, so the price is
    /// ONE MORE DRAW CALL and two triangles, plus the shading of the pixels it
    /// fills (measured: +0.15 ms on the rig in the worst view, nothing when it
    /// is occluded or at rest). The document never hears about it: no node, no
    /// outliner row, nothing in scene.bounds and nothing saved.
    ///
    /// WHY NOT THE OBVIOUS TWO (measured, spikes/gf1-ground/):
    ///   * a BIGGER default ground re-opens exactly what lane L3 closed. At 24x
    ///     (2.4 km) the automatic lit volume of a default scene collapsed from
    ///     +-23.1 m to +-1.8 m and the voxel size from 0.362 m to 0.029 m: the
    ///     ground becomes such an outlier that giItemBounds' trim drops it, so
    ///     the floor stops being lit and stops bouncing.
    ///   * the ground FOLLOWING the camera moves still geometry the probes
    ///     capture: the move raised `lastStaleReason = moved` on Showroom 2's
    ///     32-probe grid (192 cube faces to re-capture) and, in a scene the
    ///     ground dominates, drags the automatic volume along with it.
    void syncGroundHorizon();
    /// The default floor's own UV map, fitted over its ENGINE-SIDE vertices
    /// (u = ux*x + uc, v = vz*z + vc) so the horizon's checker crosses the
    /// floor's edge in phase — density, offset and sign alike, whatever the
    /// importer did to them. False when the mesh has no usable linear map.
    bool fitGroundUvMap(iris::Mesh *mesh, float &ux, float &uc, float &vz, float &vc);
    void syncGiVolume();
    jahshaka::engine::MeshId wireMeshFor(int kind);
    /// The per-sync document walk. Takes a RAW node and iterates children
    /// through iris::graph rather than through SceneNode::children(), which
    /// materialises a QList<QSharedPointer> — one heap allocation and one
    /// atomic refcount per child — for every node of the scene, every frame.
    /// `parentShown` is the parent's EFFECTIVE visibility (it and every
    /// ancestor visible): the walk is parent-first, so the rule costs one AND
    /// per node, never an ancestor walk.
    /// `parentMovable` is the parent's RESOLVED mobility (§3.3.2 rule 2),
    /// threaded down the walk so the resolution stays O(nodes).
    /// What one node's visit concluded, so the recursion can thread it down.
    struct VisitResult {
        bool shown = false;      ///< this node's EFFECTIVE visibility
        bool movable = false;    ///< ...and its resolved mobility
        bool descend = true;     ///< false where the old walk returned early
    };
    /// ONE NODE, given its parent's answers. The body of the old visit() with
    /// the child recursion taken out — the one piece of surgery the dirty set
    /// needed (DIRTY_SET_MIRROR_SPEC §4). Every latch is unchanged.
    VisitResult visitNode(iris::SceneNode *node, bool parentShown, bool parentMovable);
    /// visitNode + the recursion: THE FULL WALK, which now runs only at the
    /// explicit triggers and as the verifier's oracle.
    void visit(iris::SceneNode *node, bool parentShown, bool parentMovable);
    /// ONE MARKED NODE. Resolves its parent's answers from the DOCUMENT
    /// (isVisibleInScene / resolvedMobility, both O(depth)) rather than from a
    /// walk, so the order the change list happens to be in cannot matter.
    void visitDirty(iris::SceneNode *node);
    /// The parent's effective visibility / resolved mobility, for visitDirty.
    bool parentShownOf(iris::SceneNode *node);
    bool parentMovableOf(iris::SceneNode *node);
    /// Both answers in one memoised probe (bit 0 shown, bit 1 movable).
    quint8 parentStateOf(iris::SceneNode *node);

    /// Releases the entries of nodes that have left the document — what
    /// replaced removeMissing()'s stamp sweep over every entry in the scene.
    void consumeEvicted();
    /// Visits the change list. Materials that moved are folded in first (a
    /// material edit moves no node, so the node list cannot see one).
    void consumeDirty();
    /// The handful of things that legitimately cost something every frame: a
    /// tracking camera's focus smoothing, and the light / camera / decal
    /// HELPERS, whose geometry follows a world transform an ancestor can move
    /// without the node itself being written. All bounded by the scene's own
    /// light and camera registries, never by node count.
    void syncPerFrameSet();
    /// The amortised verifier: `mVerifierBudget` entries a sync, on a rotating
    /// cursor. Any push it makes is a MISSED MARK and is counted.
    void runVerifier();
    /// Folds material revisions into the change list (§3.6).
    void markChangedMaterials();
    /// Queues every node drawing `material` for a visit this sync.
    void markMaterialUsersDirty(iris::Material *material);
    /// Queues a character's skinned pieces for a visit this sync (a piece
    /// joining or leaving changes the union rig every other piece binds).
    void markPiecesDirty(const QVector<iris::SceneNode *> &pieces);
    /// The light-derived aggregates, folded over Scene::lights instead of over
    /// the walk (§3.7): the sun, whether anything casts, the strongest filter
    /// and the largest shadow resolution asked for.
    void refreshLightAggregates();
    /// Queues every decal node for a visit (the helpers toggle).
    void markDecalsDirty();
    /// ONE ENGINE PUSH, counted — the oracle's instrument. `what` names the
    /// latch for the one-line report a verifier catch prints.
    void notePush(const iris::SceneNode *node, const char *what);
    /// This entry's contribution to the refractive / distortion counts (§3.7).
    void noteRefractive(Entry &e, bool refractive, bool distortion);
    /// ...and to the rig counts syncSkeletonSharing / syncClips early-out on.
    void noteRigCounts(Entry &e);
    /// Moves `node` between the two materials' user lists (§3.6).
    void noteMaterialUser(iris::SceneNode *node, iris::Material *from, iris::Material *to);
    /// The mobility half of visit(): resolve, push on change, and watch for the
    /// play-time surprise mover. Returns what this node RESOLVED to, which its
    /// children inherit.
    bool syncMobility(Entry &e, iris::SceneNode *node, bool parentMovable);
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
    ///
    /// Stage 5 (AVATAR_LOCOMOTION_SPEC §7.3) added the second source: an avatar
    /// pushes the N weighted clips its locomotion state machine published this
    /// step, each at its own absolute time. See the comment at the definition
    /// for why that is a PURE TRANSLATION and can never become a cache.
    void syncClips();
    /// One entry's bones in world space, APPENDED to `out` (it is not cleared —
    /// the whole-scene overload accumulates every rig into one map). False when
    /// the entry is not a posed skinned mesh.
    bool entryBoneWorldTransforms(const Entry &e, QHash<QString, iris::Mat4> &out) const;
    /// Translates and attaches a node's clips. Idempotent: does nothing unless
    /// the rig or the clip set changed.
    void attachClipsFor(Entry &e);
    /// The six world-axis faces of an equirect panorama as engine textures the
    /// caller owns (and destroys). The SKY no longer uses it — the engine
    /// captures its own sky on the GPU (SKY-GPU) — but the per-material
    /// reflection override still projects an authored panorama here.
    bool buildEquirectCubeFaces(const QImage &equirect, jahshaka::engine::TextureId ids[6]);
    jahshaka::engine::MeshId     meshFor(iris::Mesh *mesh, const QString &rigId = QString());
    jahshaka::engine::MaterialId materialFor(iris::Material *material);
    struct MaterialSync;
    void syncTextures(Entry &e, const MaterialSync &ms);
    jahshaka::engine::TextureId textureFor(const QString &path, bool srgb);
    /// The reflection SLOT takes a cubemap, so its bind cannot go through
    /// textureFor: the six faces are built here (from the document texture's
    /// own cube faces, or by projecting an equirect image the way the sky
    /// does) and handed to Scene::createCubemap, which owns the left-handed
    /// remap. Cached in mTextures like every other bind so reclaimUnused
    /// frees it (ADDENDUM A-5).
    jahshaka::engine::TextureId reflectionCubeFor(const QString &path);
    /// LIVE TEXTURES (ADDENDUM A-1): one pass per sync that re-uploads only the
    /// live textures whose document GENERATION moved since the last upload. A
    /// still live texture costs one hash walk and one integer compare — no
    /// engine call, no allocation, nothing per frame.
    void syncLiveTextures();
    /// Reads a document material into PBR parameters. Public for tests.
public:
    static bool toPbrParams(iris::Material *material, jahshaka::engine::PbrParams &out);
    /// Records that this material is refractive, for the chain's Auto mode.

    /// Re-arms the per-Item state the mirror owns after the engine re-created
    /// the renderables of every node using `material` (a shading-model switch).
    void onMaterialItemsRebuilt(jahshaka::engine::MaterialId material);
    static jahshaka::engine::LightDesc toLightDesc(iris::LightNode *light);
    /// The same, with the scene's SUN already resolved by the caller —
    /// `sunKnown` says the caller did resolve it (a null `sun` then means "this
    /// scene has no sun", not "ask the document"). The per-sync walk resolves
    /// once for every light it pushes: the document answers by building and
    /// sorting a QVector of every directional, and that ran per directional
    /// light per frame (clean-2 lane, 2026-09-13).
    /// `sunTint` is the ATMOSPHERE'S TINT on this light's colour — white for
    /// every light but the sun, and white for the sun too unless it follows the
    /// atmosphere and the atmosphere is the sky (atmosphereTintFor). It is a
    /// PARAMETER rather than a lookup so this stays a pure function of its
    /// inputs: the per-sync walk and the full-walk verifier must derive the
    /// same description from the same node.
    static jahshaka::engine::LightDesc toLightDesc(
        iris::LightNode *light, iris::LightNode *sun, bool sunKnown,
        const jahshaka::engine::Colour &sunTint =
            jahshaka::engine::Colour(1.0f, 1.0f, 1.0f, 1.0f));
    /// THE SUN'S TINT (SUN_FOLLOWS_ATMOSPHERE, lane ENGINE-7 item 6): what the
    /// air does to this light's colour at its own elevation, asked of the
    /// renderer's own sky model (Scene::atmosphereSunTint). White unless
    /// `light` IS the scene's sun, the sun's "Follows Atmosphere" is on, and
    /// the scene's sky is the analytic atmosphere — the engine answers the last
    /// of those itself. ONE value feeds the light and the sun DISC, so the two
    /// can never disagree about what colour the sun is.
    jahshaka::engine::Colour atmosphereTintFor(const iris::LightNode *light,
                                               const iris::LightNode *sun) const;
    /// Re-pushes the SUN's light description when its atmosphere tint moved.
    /// The dirty set cannot carry this one: the tint follows the sun's
    /// TRANSFORM, and a transform write marks nothing (the light rides the
    /// adopted node). One light, one compare, per sync.
    void syncSunAtmosphere();
    /// Fills everything but the texture ids (those need the atlas).
    static jahshaka::engine::DecalDesc toDecalDesc(iris::DecalNode *decal);
    /// The document -> engine particle mapping (PARTICLES_FX2_SPEC §5), isolated
    /// so the suites can assert on the desc without a scene or a frame. Static
    /// for the same reason toLightDesc is: it reads the node and nothing else.
    static jahshaka::engine::ParticleSystemDesc toParticleDesc(
        iris::ParticleSystemNode *ps, jahshaka::engine::TextureId tex);

    /// THE RENDER-LOOP MONITOR's host hook (RENDER_LOOP_MONITOR_SPEC §4.2,
    /// lane MON-P1b). The mirror is the biggest single piece of host work in a
    /// frame, and "the mirror" as ONE number says nothing — a capture needs to
    /// see the walk, the material pass, the live-texture uploads and the clip
    /// push apart. Give it the engine once (the host that owns both) and
    /// sync() reports its sub-stages into the frame record the engine is
    /// building; leave it null and nothing here reads a clock.
    ///
    /// COSTS ONE VIRTUAL CALL PER SYNC when no capture is running
    /// (Engine::frameMonitor(), checked once at the top of sync), and nothing
    /// else. Not owned; cleared by passing null.
    void setMonitorEngine(jahshaka::engine::Engine *engine) { mMonitorEngine = engine; }

private:
    jahshaka::engine::Engine *mMonitorEngine = nullptr;

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
    /// MEMO of the two things that depend only on the MATERIAL, not on the
    /// node: its PbrParams and its texture-bind signature. Both used to be
    /// recomputed per MESH per frame — `toPbrParams` runs two dynamic_casts and
    /// a scan of every shader property, and `syncTextures` did seven
    /// QHash<QString> lookups whose keys it built from `const char *` (a QString
    /// construction each) plus a QVector of binds — so a scene of 8000 cubes
    /// sharing ONE material paid for that material 8000 times a frame.
    ///
    /// IT SURVIVES THE FRAME (MIRROR_SCALE lane, 2026-09-13). Sharing was only
    /// half the problem: the editor gives every primitive its OWN PbrMaterial
    /// (SceneEditService::addNodeToScene), so a scene of 8000 cubes is a scene
    /// of 8000 materials and a per-SYNC memo rebuilt all 8000 descriptions on
    /// every still frame — ~13 heap allocations each (a std::string for the
    /// BRDF name, ten QStrings for the address rows, the bind vector). Measured
    /// on a 2000-node lattice: 4.08 us per node per still frame with its own
    /// material per node against 0.83 us with one shared material — four fifths
    /// of the walk was this function.
    ///
    /// WHAT MAKES REUSE SAFE is `fingerprint`: an FNV over every document field
    /// the two builds read, computed once per material per sync out of PODs
    /// with no allocation at all. A material whose fingerprint has not moved
    /// cannot have produced different output, so the memo stands. A document
    /// REVISION COUNTER would be cheaper still and was rejected: PbrMaterial's
    /// parameters are public fields written from panels, scripts, readers and
    /// commands, and a counter is only as good as the writer that remembers to
    /// bump it — a forgotten one is an edit that silently never reaches the
    /// renderer. The fingerprint reads the same fields the conversion does, so
    /// it cannot go stale that way; what it CAN do is forget a NEW field, which
    /// is what mirror.scale's "every authored property moves the fingerprint"
    /// case (driven from PbrMaterial's own property rows) exists to catch.
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
        /// Every document field the build above read, as one hash. See the
        /// block comment: this is what lets the memo cross frames.
        quint64                       fingerprint = 0;
        /// The sync that last validated this entry. Two jobs: a material is
        /// fingerprinted at most ONCE per walk however many nodes share it,
        /// and an entry the walk did not reach is dropped at the end of it
        /// (so the hash never holds a pointer to a material that has left the
        /// document — the keys are raw, like mMaterials' own).
        quint32                       lastSeen = 0;
        /// The MATERIAL'S OWN REVISION when this description was built
        /// (iris::Material::revision — DIRTY_SET_MIRROR_SPEC §3.6). THE FAST
        /// PATH: the fingerprint below is still computed, but only when this
        /// number moved, and by the verifier. A material per primitive means
        /// 8,404 fingerprints a frame on the render review's lattice, which is
        /// ~4 ms the dirty design cannot afford; a compare of one quint32 is
        /// what a still frame pays instead — and nothing at all, because a
        /// still frame does not reach this function.
        quint32                       revision = 0;
        /// The dynamic_cast, resolved once per material instead of twice per
        /// mesh per frame. Null for a material that is not a PbrMaterial.
        iris::PbrMaterial            *asPbr = nullptr;
        /// THE PUSH GUARD: the engine material this description was last
        /// pushed to, and the fingerprint it was pushed at. setPbrMaterial
        /// re-applies the whole datablock (a const-buffer upload), so the
        /// mirror still LOOKS every frame and pushes only a change.
        jahshaka::engine::MaterialId  pushedTo = 0;
        quint64                       pushedFingerprint = 0;
        bool                          pushed = false;
    };
    QHash<iris::Material *, MaterialSync> mMaterialSync;
    /// The PBR state last PUSHED to each engine material, and the guard that
    /// keeps setPbrMaterial off the per-frame path. Keyed by engine MaterialId
    /// because that is what the push targets; engine ids are never reused (the
    /// counter only increments), so a destroyed material cannot inherit a stale
    /// record — it is dropped in reclaimUnused anyway.
    /// (mPbrPushed — a second QHash, keyed by engine material id, holding a
    /// whole PbrParams copy per material — is GONE with the MIRROR_SCALE lane.
    /// It compared a full PbrParams (std::string BRDF name included) and cost a
    /// hash probe per MESH NODE per frame to do it; the same statement now
    /// lives in the memo above as `pushedTo` + `pushedFingerprint`, in the
    /// entry the walk already has in hand.)
    /// How many times each engine material has had its Items REBUILT under the
    /// entries drawing it (a shading-model switch). Bumped by
    /// onMaterialItemsRebuilt; an entry whose `materialItemSerial` is behind
    /// re-pushes its query flags on its next visit. See that function for what
    /// this replaced (an O(entries) walk per material, i.e. O(N^2) per open).
    QHash<jahshaka::engine::MaterialId, quint32> mMaterialItemSerial;
    const MaterialSync &materialSyncFor(iris::Material *material);
    /// The memo's validity key — see MaterialSync. `pbr` is the resolved cast
    /// (null when the material is not a PbrMaterial), so this never runs one.
    static quint64 materialFingerprint(iris::Material *material, iris::PbrMaterial *pbr);
    /// Binds a graph material's generated shader pieces (HLMS_ADOPTION P5).
    void syncCustomPieces(iris::Material *material, jahshaka::engine::MaterialId id);
    /// True once any mirrored material has carried a generated piece: the gate
    /// on pushing the shader clock at all, so a scene without one is untouched.
    bool mAnyCustomPiece = false;
    /// The host's clock for generated pieces. Negative = use the wall clock
    /// below; a test or a timeline sets an exact value through
    /// setShaderTimeOverride so a frame is reproducible.
    float mShaderTimeOverride = -1.0f;
    QElapsedTimer mShaderClock;
    /// FOCUS SMOOTHING's clock and the seconds it produced for THIS sync
    /// (CAMERA_LENS_SPEC §3 P2). One clock for the whole walk, not one per
    /// camera, so every tracking camera eases by the same dt in a frame. Zero
    /// on the first sync (nothing to ease from) and clamped, so a stalled
    /// editor does not teleport focus on the frame it wakes up. The smoothing
    /// arithmetic itself is a pure function of this dt
    /// (iris::lens::smoothTowards) precisely so it can be tested without one.
    QElapsedTimer mFocusClock;
    float         mFocusDt = 0.0f;
    /// Socket attachments (CAMERAS_SPEC §5). Owns the reused scratch buffers;
    /// its pose source is this mirror, installed by the constructor.
    /// One character's union rig, derived and cached (AVATAR_RIG_PERF_SPEC
    /// §3.1). Keyed by the CHARACTER HOST — the nearest ancestor carrying a
    /// skeletal clip, or the pieces' common parent when there is none.
    struct CharacterRig {
        /// The pieces' skeleton pointers, in document order, hashed: the union
        /// is re-derived only when the character's piece set really changes.
        quint64 signature = 0;
        /// Bumped whenever the derived rig id changes. Entries compare it.
        quint32 epoch = 0;
        const iris::SceneNode *host = nullptr;   ///< the key, for the entries
        iris::SkeletonPtr rig;              ///< null = this character has one piece
        std::string rigId;                  ///< the union's SkeletonDesc id
        /// Per PIECE skeleton: its blend index -> union bone index map. A piece
        /// missing from this map is one the union excluded; it keeps its own rig.
        QHash<const iris::Skeleton *, QVector<unsigned short>> remaps;
    };
    QHash<const iris::SceneNode *, CharacterRig> mCharacterRigs;
    /// The character a piece belongs to: the nearest ancestor-or-self carrying a
    /// skeletal clip (the same walk clip translation uses), or the piece's own
    /// parent when the character has no clips at all. Null when that parent is
    /// the scene ROOT — two unrelated single-piece characters dropped side by
    /// side in a scene are not one character, and unioning their rigs would
    /// merge two strangers' skeletons.
    const iris::SceneNode *characterHostOf(iris::SceneNode *node) const;
    /// The character rig for a piece, derived if needed. Null when the piece is
    /// alone (or was excluded from the union) — then it keeps its own rig.
    const CharacterRig *characterRigFor(iris::SceneNode *piece);
    /// Arms (and disarms) skeleton sharing for every multi-piece character in
    /// the scene — §3.4. Runs once per sync, BEFORE syncClips, and does nothing
    /// at all in a scene with no multi-piece character.
    void syncSkeletonSharing();
    /// The per-sync grouping scratch: character host -> its mirrored pieces.
    /// A MEMBER so the steady state allocates nothing.
    QHash<const iris::SceneNode *, QVector<Entry *>> mShareGroups;

    /// THE SOCKET RECONCILER (AVATAR_RIG_PERF_SPEC §4.2): keeps the ENGINE's
    /// tag points equal to the DOCUMENT's socket attachments, instead of moving
    /// riders itself every frame. Returns how many riders are being driven.
    int reconcileSockets();
    /// Frees the tags of riders the document no longer attaches. Runs at the
    /// END of sync, after removeMissing has dropped the entries of deleted
    /// nodes — the map below is keyed by document node pointer.
    void sweepStaleRiders();
    /// Takes EVERY rider off its bone — the document's graph is leaving this
    /// scene manager (setSource, evacuateEngineObjects), and a node hanging off
    /// one of its TagPoints would not travel with the tree.
    void releaseAllRiders();
    /// Takes one rider off its bone (if it is on one) and forgets it. The pose
    /// it was last resolved to is baked into its local under its document
    /// parent, UNLESS the caller has written that local since the last sync.
    void releaseRider(iris::SceneNode *rider);
    /// What the engine was last asked for, per rider — so an unchanged socket
    /// costs one comparison and no engine call.
    struct RiderState {
        jahshaka::engine::NodeId owner = 0;
        QString bone;
        quint64 offsetKey = 0;
        /// THE AUTHORED LOCAL, kept across a spell on the fallback path.
        ///
        /// With no engine rig to hang a tag on, a rider is driven the old way:
        /// its WORLD transform is written every sync. That write lands in the
        /// very field D4 gave a new meaning to — the rider's local, which is now
        /// its offset FROM the socket — so a rider that spent one sync on the
        /// fallback (every socketed node does, between the document attaching it
        /// and the walk rigging its owner) would come out of it carrying a world
        /// transform as its socket offset, and ride two units above the bone
        /// forever. The authored value is snapshotted on the way in and restored
        /// when the tag arms.
        bool fallbackDriven = false;
        iris::Vec3 authoredPos;
        iris::Quat authoredRot;
        iris::Vec3 authoredScale{1, 1, 1};
        /// Is `authored*` a real reading? (round-2 review, item 3.) It is
        /// recorded on every sync the rider spends ON ITS TAG, where its local
        /// IS the offset — because by the time the rig goes away the local is
        /// no longer trustworthy: the ENGINE frees every rider on a skeleton
        /// it rebuilds, and Ogre's own detach re-expresses the node's local as
        /// it does so. Reading the offset at that point gave whatever the
        /// detach left (measured: zero), and the re-arm restored THAT.
        bool authoredValid = false;
        /// The rider's local TRS as of the last sync that looked at it. What it
        /// is FOR: when the attachment goes away, the rider "keeps the pose it
        /// was last resolved to" — so its world transform is baked into its new
        /// local under its document parent. But a caller that detaches and then
        /// PLACES the node ("detach is also how you put something where a bone
        /// was, and then move it") writes its local before the next sync, and
        /// baking the old world over that write would silently lose it. So the
        /// bake happens only when the local is still the one the mirror last
        /// saw: an explicit write wins.
        iris::Vec3 lastLocalPos;
        iris::Quat lastLocalRot;
        iris::Vec3 lastLocalScale{1, 1, 1};
        /// THE FALLBACK PUSH'S CHANGE GUARD (lane ENGINE-7 item 4). A hash of
        /// the 16 floats of the world transform the fallback last wrote. With
        /// no engine rig the rider is placed by WRITING its world every sync —
        /// through the document's marking setters, so a still scene holding a
        /// socketed prop bumped the transform-write epoch on every frame and
        /// re-ran every walk hanging off it (nodegraph.h). The write happens
        /// when the socket's world really moved, or when something else wrote
        /// the rider's local since ours (lastLocal* above is that test), and
        /// not otherwise.
        quint64 fallbackWorldKey = 0;
        bool    fallbackWorldPushed = false;
    };
    QHash<const iris::SceneNode *, RiderState> mBoneRiders;
    /// The riders the reconciler saw this sync — what the end-of-sync sweep
    /// measures "stale" against. A member so the steady state allocates nothing.
    QSet<const iris::SceneNode *> mRidersSeen;
    int                      mSocketDangling = 0;

    iris::SocketResolver     mSockets;
    /// Counts every setClipStates call this mirror makes (clipStatePushes()).
    quint64                  mClipStatePushes = 0;
    /// entryBoneWorldTransforms' scratch. Members because sockets made that
    /// function per-frame work (it used to run only when the bone overlay
    /// refreshed) — see the note at its assign() calls.
    mutable std::vector<jahshaka::engine::BonePose> mPoseScratch;
    mutable std::vector<iris::Mat4>                 mDerivedScratch;
    mutable std::vector<char>                       mDerivedDone;
    /// syncClips' two reused buffers, members for the same reason: it runs for
    /// every skinned node every frame, and a blend space publishes a fresh
    /// weight set on all of them (Stage 5). Reusing the buffers keeps their
    /// capacity — and the ClipState strings' capacity — instead of allocating
    /// per avatar per frame.
    QVector<Entry::ClipPush>                 mClipPushScratch;
    std::vector<jahshaka::engine::ClipState> mClipStateScratch;
    /// entryBoneWorldTransforms' FK step, hoisted out of a per-frame
    /// heap-allocating std::function. Fills mDerivedScratch[i] (and its
    /// ancestors) from `parents` and returns it.
    const iris::Mat4 &resolveBoneDerived(int i, const std::vector<int> &parents,
                                        const std::vector<jahshaka::engine::BonePose> &poses) const;
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
    ///
    /// KEYED BY (mesh, RIG ID) since the character rig landed
    /// (AVATAR_RIG_PERF_SPEC §3.1). An Ogre Mesh holds exactly ONE SkeletonDef
    /// and exactly one blend-index map (they live on the SubMesh), so one engine
    /// mesh cannot serve two different rigs — and two characters CAN legitimately
    /// share a document mesh asset while resolving to different union rigs (one
    /// has a hair piece, the other does not). The rig id is empty for every
    /// unskinned mesh, so nothing but skinned content is affected: an unskinned
    /// mesh is cached exactly as it was.
    QHash<QPair<iris::Mesh *, QString>, jahshaka::engine::MeshId> mMeshes;
    // (A second `mPoseScratch` lived here, unused since the document's pose PUSH
    // was retired with its clip evaluator — deleted rather than shadowed by the
    // read-back scratch above, which is a live buffer with the same name.)
    QHash<iris::Material *, jahshaka::engine::MaterialId> mMaterials;
    QHash<QString, jahshaka::engine::TextureId> mTextures;
    /// For every LIVE entry of mTextures (same key), the document generation
    /// its pixels were last uploaded from. A key is here only while its engine
    /// texture is; reclaimUnused drops both together.
    QHash<QString, quint64> mLiveGenerations;
    QHash<QString, jahshaka::engine::TextureId> mIconTextures;   // light icon glyphs (Qt resources)
    jahshaka::engine::MaterialId mDefaultMaterial = 0;
    bool mLightWires = true;
    bool mCameraBodies = true;
    /// The camera currently DRIVING the view (applyCamera's last one, after the
    /// active-camera substitution). Its own body and frustum are suppressed —
    /// see applyCamera. Raw pointer, compared only for identity; the document
    /// owns it and a stale value can only ever fail to match.
    const iris::CameraNode *mViewCamera = nullptr;
    /// PER VIEW, which mViewCamera cannot be: one mirror serves the viewport, a
    /// screenshot's throwaway view and the player's, so "did the driving camera
    /// change" is only answerable per view (CAMERA_LENS_SPEC §4, the camera-cut
    /// re-seed). A bounded LRU — see noteDrivingCamera — because those views
    /// come and go. Both pointers are compared only for identity and are never
    /// dereferenced.
    /// WEAK on the camera: the entry outlives the call that wrote it and is
    /// dereferenced a frame later, so a deleted camera must read as "gone" and
    /// not as a dangling pointer.
    std::vector<std::pair<const jahshaka::engine::View *, QWeakPointer<iris::CameraNode>>>
        mDrivingCameras;
    /// THE WORLD'S post description as applyEnvironment last built it, BEFORE
    /// the driving camera was layered over it. The picture-in-picture inset is
    /// a different camera's shot, so it grades from this and lays the PIPPED
    /// camera over it (applyPip) — inheriting the view's description instead
    /// would give the pipped camera whatever exposure the camera driving the
    /// MAIN view happened to ask for. Scene-level, so one value serves every
    /// view: the fields it carries (hdr, exposure) come from the document.
    jahshaka::engine::PostFxDesc mWorldPostFx;
    /// WHAT THE DOCUMENT'S SKY IS MADE OF, as one value — the question "does
    /// anything have to be BAKED again?" and nothing else. applySky DISPATCHES
    /// on the kind (and the realistic-bake debounce asks "was the previous sky
    /// also realistic?"); the parameters only ever need an equality test.
    ///
    /// Only the fields the kind actually uses are ever filled, so member-wise
    /// equality asks exactly what the per-kind 64-bit hash this replaced asked
    /// — without the hash's (small) collision story, and without the ten
    /// QString::arg calls per frame that came before the hash. It is
    /// deliberately NOT the engine-side SkyDesc: that one is made of texture
    /// ids, which only exist AFTER the bake this comparison decides to skip.
    ///
    /// The SKY LIGHT is deliberately absent: its intensity and tint scale the
    /// recorded integral in applyEnvironment, they do not change the integral,
    /// so folding them in here would re-bake the whole sky to answer a question
    /// the bake does not affect.
    struct SkySource {
        enum class Kind { None, Equirect, Cubemap, Gradient, Realistic, Color };
        Kind    kind = Kind::None;
        /// Equirect: the image's path (the texture cache is keyed by it too).
        QString equirectPath;
        /// Cubemap: the document texture's identity. The six face images are
        /// not comparable cheaply and a document texture is never mutated in
        /// place — swapping the sky swaps the object.
        const void *cubeTexture = nullptr;
        /// Gradient: the three stops and the horizon offset.
        QColor  gradientTop, gradientMid, gradientBot;
        float   gradientOffset = 0.0f;
        /// Realistic: the analytic sky's five parameters. They are the
        /// ENGINE's (SKY-GPU) — pushed into Ogre's AtmosphereNpr, evaluated per
        /// pixel on the GPU — so a change here is a const-buffer write and a
        /// re-capture of the environment, never a CPU bake. There is no
        /// debounce any more for exactly that reason.
        float   density = 0.0f, diffusion = 0.0f, horizon = 0.0f, power = 0.0f;
        QColor  skyColour;
        /// Realistic: the SUN ray's air (the atmosphere's turbidity). It is a
        /// sky-source field because it lives on the sky block, but it changes
        /// no sky pixel — it is the only input to the engine's sun-tint model.
        float   sunHaze = 0.0f;
        /// Realistic: the SUN LIGHT's direction (towards the sun) and whether
        /// the scene has one at all — the sky's only sun input since D15.
        /// Compared with a dot-product band rather than exactly, so a gizmo
        /// drag's float noise does not re-capture the environment every frame.
        iris::Vec3 sunDir;
        bool    hasSun = false;
        /// Color: a SINGLE_COLOR sky is a REAL sky now (SKY_LIGHT_SPEC §2) —
        /// baked as a 64x32 strip of the colour and pushed through the equirect
        /// path, so its SH band 0 is linear(colour), its reflections are
        /// uniform, and the sun disc has a sky pass to compose over.
        QColor  skyColor;

        bool operator==(const SkySource &o) const;
        bool operator!=(const SkySource &o) const { return !(*this == o); }
    };
    /// Reads the document's sky fields into the value above.
    static SkySource skySourceOf(const iris::Scene &scene);
    SkySource mSkySource;
    /// The sky description the engine holds (Scene::setSky is idempotent, so
    /// applySky pushes this every frame and lets the boundary drop it). Built
    /// once per SkySource change, beside the bake that produced its textures.
    jahshaka::engine::SkyDesc mSkyDesc;
    /// The equirect sky's texture, taken from the shared cache (unlike the
    /// cubemap/gradient/realistic paths, which upload their own). Held so
    /// reclaimUnused does not free what the engine's sky is sampling.
    jahshaka::engine::TextureId mSkyTexture = 0;
    jahshaka::engine::TextureId mSkyFaceTextures[6] = { 0, 0, 0, 0, 0, 0 };
    // Last ambient pair actually pushed. Ogre picks its ambient shader variant
    // from these (equal => fixed, different => hemisphere), so pushing an
    // unchanged value every frame is not free.
    bool mAmbientPushed = false;
    /// The environment light's gain, pushed on change beside the coefficients
    /// (SMOKE-ENGINE-1 item 2). Separate from mAmbientPushed because the two
    /// move independently: a sky change moves the coefficients and not the
    /// gain, a Sky Light intensity change moves both.
    float mLastEnvScale = 0.0f;
    bool mEnvScalePushed = false;
    float mLastAmbientSh[27] = { 0.0f };
    /// THE SKY'S OWN LIGHT (SKY_LIGHT_SPEC.md §2), READ FROM THE ENGINE
    /// (SKY-GPU): the cosine-convolved integral of the sky the engine just
    /// drew, in linear light, refreshed from Scene::skyAmbientSh each frame and
    /// scaled here by the scene's Sky Light. The host has no sky image to
    /// integrate any more — the analytic sky is a shader and an HDRI's integral
    /// was the biggest CPU lighting computation left (CPU_GPU_LIGHTING_AUDIT
    /// F2). No Sky Light => 27 zeros, whatever this holds.
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
    /// The visibility last PUSHED to the grid's node, -1 = never. Same reason
    /// as the GI boxes above: setNodeVisible is a subtree walk and the grid
    /// node has two children.
    int   mGridVisiblePushed = -1;
    GridPlane mGridPlane = GridPlane::Floor;
    GridPlane mGridBuiltPlane = GridPlane::Floor;
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
    // The ground's horizon (syncGroundHorizon). `mHorizonFloor` is the default
    // floor this walk found — a RAW pointer, valid only for the walk that set
    // it, which is why the sync reads it through mEntries and never dereferences
    // it after the walk.
    const iris::MeshNode *mHorizonFloor = nullptr;
    /// The floor MESH the horizon's UV map was fitted from; a floor that
    /// changes mesh rebuilds the quad against the new map.
    const iris::Mesh *mHorizonMeshSource = nullptr;
    jahshaka::engine::NodeId mHorizonNode = 0;
    jahshaka::engine::MeshId mHorizonMesh = 0;
    jahshaka::engine::MaterialId mHorizonMaterial = 0;
    int mHorizonVisible = -1;
    iris::Mat4 mHorizonWorld;     ///< the floor transform last pushed (nothing at rest)
    /// The document's transform-write count when the horizon's world was last
    /// resolved. Nothing wrote a transform => the floor cannot have moved, and
    /// the derived-transform walk below can be skipped entirely.
    unsigned long long mHorizonWrites = ~0ull;
    // The GI volume overlay: one node per box, rebuilt only when the reported
    // bounds actually move (a GI rebuild is rare; this sync runs every frame).
    bool mGiVolumeVisible = false;
    jahshaka::engine::NodeId mGiVolLitNode = 0, mGiVolProbeNode = 0;
    jahshaka::engine::MeshId mGiVolLitMesh = 0, mGiVolProbeMesh = 0;
    jahshaka::engine::MaterialId mGiVolLitMaterial = 0, mGiVolProbeMaterial = 0;
    jahshaka::engine::Vec3 mGiVolLitMin, mGiVolLitMax, mGiVolProbeMin, mGiVolProbeMax;
    bool mGiVolBuilt = false;
    /// The visibility last PUSHED to each GI volume box, -1 = never
    /// (MIRROR_SCALE lane). An engine setNodeVisible is a subtree walk, and
    /// both boxes were re-hidden every frame in every scene that never shows
    /// them — which is every scene, until somebody opens the GI overlay.
    int mGiVolLitVisible = -1;
    int mGiVolProbeVisible = -1;
    /// The highlighted SET, primary first. Empty = nothing selected.
    QList<iris::SceneNodePtr> mHighlighted;
    /// The same set, for the membership test (isHighlighted). The list keeps
    /// the ORDER and the strong references; this answers "is this one in it?"
    /// without a scan, which the walk asks per light and per camera.
    QSet<const iris::SceneNode *> mHighlightSet;
    /// The set's PRIMARY member, or null. Only distinguishes a colour when the
    /// set has more than one member (see setHighlightedNodes).
    iris::SceneNodePtr mHighlightPrimary;
    /// The primary AS ASKED FOR, which is not the same as the one in force: a
    /// primary the caller filtered out of the list colours nobody. Kept so that
    /// re-asserting the SAME selection (which the viewport does every frame) is
    /// recognisably no change at all.
    iris::SceneNodePtr mHighlightRequestedPrimary;
    /// One highlight shell per mesh under the highlighted node: selecting an
    /// asset's root outlines the whole subtree. Pooled and reused across frames.
    struct HighlightShell {
        jahshaka::engine::NodeId node = 0;
        jahshaka::engine::MeshId mesh = 0;   // engine mesh currently attached
        bool wireframe = false;              // which material the shell carries
        /// Whether the shell is the SKINNED silhouette (Pbs-backed, riding the
        /// character's skeleton) and, if so, whose skeleton it rides. Both are
        /// part of the shell's identity: a target that becomes (or stops being)
        /// GPU-skinned, or whose engine node was rebuilt, needs a re-attach.
        bool skinned = false;
        jahshaka::engine::NodeId master = 0;
        /// Whether the shell currently carries the PRIMARY (brighter) material.
        /// Part of the shell's identity for the same reason `skinned` is: a
        /// pooled shell that moves between the primary and a secondary needs a
        /// re-attach, not just a transform push.
        bool primary = false;
        /// Whether the engine currently shows this shell. setNodeVisible is not
        /// free and the answer changes only when the selection does.
        bool shown = false;
        /// The world-transform signature (plus the outline width) the shell's
        /// transform was last derived from; the derive is a full world matrix,
        /// a decomposition and an engine write, and a standing selection needs
        /// none of it. `transformPushed` is false until the first one lands.
        quint64 transformKey = 0;
        bool transformPushed = false;
    };
    std::vector<HighlightShell> mHighlightShells;
    /// One entry per mesh the highlight has to shell, and which HALF of the
    /// selection it came from — `primary` picks the brighter material.
    struct HighlightTarget {
        iris::MeshNode *node = nullptr;
        jahshaka::engine::MeshId mesh = 0;
        bool primary = false;
    };
    /// syncHighlight's per-frame target list. A member so the walk over the
    /// selected subtree reuses its storage instead of allocating a vector a
    /// frame.
    std::vector<HighlightTarget> mHighlightTargets;
    void collectHighlightMeshes(iris::SceneNode *node, bool primary,
                                std::vector<HighlightTarget> &out);
    /// The four highlight materials, indexed by (primary, skinned/wireframe).
    /// They are created lazily — a scene that never multi-selects never builds
    /// the primary pair, and a scene with no rigged mesh never builds the
    /// skinned ones.
    jahshaka::engine::MaterialId mHighlightMaterial = 0;   // wireframe (on top)
    jahshaka::engine::MaterialId mOutlineMaterial = 0;     // inverted hull
    /// The same hull for SKINNED targets: HlmsUnlit cannot skin, so a rigged
    /// character's silhouette is a Pbs datablock with the colour as emissive,
    /// sharing the character's own skeleton instance (see syncHighlight).
    jahshaka::engine::MaterialId mOutlineSkinnedMaterial = 0;
    /// ...and the PRIMARY member's three (D4 b).
    jahshaka::engine::MaterialId mHighlightPrimaryMaterial = 0;
    jahshaka::engine::MaterialId mOutlinePrimaryMaterial = 0;
    jahshaka::engine::MaterialId mOutlinePrimarySkinnedMaterial = 0;
    bool mHighlightWireframe = false;
    QColor mHighlightColourApplied;                        // what the materials show now
    QColor mHighlightPrimaryColourApplied;                 // ...and the primary's
    // Strongest shadow quality any shadow-casting light asked for, from the last
    // sync(); pushed engine-wide by applyEnvironment (see comment there).
    jahshaka::engine::ShadowFilter mShadowFilter = jahshaka::engine::ShadowFilter::Hard;
    bool mAnyShadowCaster = false;
    /// Does the scene contain a refractive material right now? Drives the
    /// chain's "Auto" refraction mode (POST_CHAIN_SPEC.md §9.5); recomputed
    /// every sync() exactly like mAnyShadowCaster.
    bool mAnyRefractive = false;
    /// ...and the same question for the DISTORTION shading model, driving the
    /// chain's "Auto" distortion mode (POST_LOOKS_SPEC.md §5.3).
    bool mAnyDistortion = false;
    // Largest shadow-map resolution any shadow-casting light asked for, from the
    // last sync(); pushed engine-wide by applyEnvironment (the engine's atlas is
    // global, like the filter — rebuild is expensive, so only on change).
    unsigned mMaxShadowResolution = 0;
    // Global illumination: last pushed state + the driving light's transform, so
    // applyEnvironment only re-pushes on change and re-traces on light movement.
    jahshaka::engine::GiParams mLastGi;
    bool mGiPushed = false;
    /// Set by invalidateEnvironment: the next applyEnvironment re-asserts the
    /// process-wide GI binding for this scene instead of re-pushing (P10).
    bool mGiReassertPending = false;
    /// The engine's material generation as last adopted (Scene::giMaterialSignature),
    /// and whether the pending settle was armed by lights/geometry — the only
    /// changes the cheap re-inject cadence serves (a material-only edit waits
    /// for the settle without it).
    quint64 mGiMaterialSignature = 0;
    bool     mGiPendingInject = false;
    /// The GI-driving light transform(s), as a CHANGE KEY rather than a
    /// matrix — see the signature's derivation in applyEnvironment (audit F8).
    quint64 mGiLightSignature = 0;
    /// applyEnvironment's ancestor-signature scratch: the lights of a scene
    /// share their parent chains, so the chain above each distinct parent is
    /// walked once per call instead of once per light. A member for its
    /// capacity only; it is cleared at the top of every use.
    std::vector<std::pair<iris::graph::NodeHandle, quint64>> mGiChainMemo;
    // Diagnostics behind giPushCount() / giRefreshCount(); never read by the
    // mirror itself, and NOT reset by invalidateEnvironment (a re-take of the
    // screen is a real push, and the gate counts real pushes).
    quint64 mGiPushCount = 0;
    quint64 mGiRefreshCount = 0;
    quint64 mGiLightRefreshCount = 0;
    /// materialBuildCount() — reset at the top of every sync, so it reports the
    /// LAST walk rather than a running total.
    quint64 mMaterialBuilds = 0;
    /// The settle machine behind the static re-promotion (see sync()).
    unsigned long long mLastTransformWrites = 0;
    /// ...and the document's demotion count when the settle last ran. A quiet
    /// spell is only worth a re-derivation if a transform write really took a
    /// subtree OUT of the static half since the last one; a camera orbit, an
    /// undo and a scene open all write transforms and demote nothing.
    unsigned long long mLastStaticDemotions = 0;
    quint32 mSettleFrames = 0;
    bool    mStaticSettlePending = false;
    quint64 mStaticRepromotions = 0;
    /// SKELETON SHARING (syncSkeletonSharing): how many CHARACTER PIECES the
    /// last walk saw — pieces of a multi-piece character, the only things that
    /// can share — and the document's transform-write count when the world
    /// comparisons were last made. Below two pieces there is nothing to share
    /// and the pass returns before it walks a single entry; with no write since
    /// the last pass no world transform can have moved.
    quint32 mCharacterPieces = 0;
    /// ...and how many GPU-SKINNED nodes it saw at all. syncClips has nothing
    /// to push below one and used to walk every entry to find out.
    quint32 mSkinnedNodes = 0;
    unsigned long long mShareWorldWrites = ~0ull;
    // ---- MOBILITY counters (REALTIME_REFLECTIONS_SPEC §3.3) ----------------
    /// Recomputed every sync (the walk resolves every node anyway), so this is
    /// a state, not a running total.
    quint64 mMovableNodes = 0;
    /// THE SCENE'S SUN, resolved once at the head of each sync and handed to
    /// every toLightDesc of that walk (clean-2 lane). Never dereferenced
    /// outside the walk that set it.
    iris::LightNode *mSyncSun = nullptr;
    quint64 mMobilityMisses = 0;
    QString mLastMobilityMiss;
    /// THE LIGHTS THIS WALK RESOLVED AS MOVING, by node pointer, valid for the
    /// rest of the frame (applyEnvironment runs after sync and the pointers
    /// cannot die in between). It exists because the GI light signature has to
    /// ask "does this lamp move?" about each light while the answer is a
    /// property of the node's whole ancestor chain, which only the walk knows.
    ///
    /// A VECTOR, cleared and refilled per sync for its CAPACITY: this is the
    /// hot per-node walk, and a node-allocating container here would be a
    /// malloc per movable lamp per frame for a list that is almost always
    /// shorter than ten. Membership is a linear scan, which at that length
    /// beats any tree.
    std::vector<const iris::SceneNode *> mMovableLights;
    /// "A node's mobility CHANGED this sync" — set by syncMobility, consumed by
    /// applyEnvironment's settle gate. A class flip moves the engine's GI
    /// geometry signature (the object enters or leaves it), and that signature
    /// is the settle key: without this flag the frame a character promotes
    /// ARMS a full re-solve, which is the hitch the whole design forbids
    /// (code review 2026-09-12, item 1). The gate ADOPTS the new signature
    /// instead — an Authoring flip's rebuild is owed on its own account
    /// (setNodeMovable invalidated), and a Soft one owes nothing at all.
    bool mMobilityChanged = false;

    // ---- THE DIRTY SET'S OWN STATE (DIRTY_SET_MIRROR_SPEC) ----------------
    /// The next sync walks the WHOLE document. True to begin with (the first
    /// sync after a bind adopts everything) and re-armed at the explicit
    /// triggers of §3.5.
    bool mFullWalkPending = true;
    /// Which mode the last sync ran in — what walkMode() reports.
    bool mLastWalkWasFull = false;
    /// Inside a verification visit: materials are re-fingerprinted rather than
    /// trusted to their revision, and the visit's pushes are counted as
    /// catches rather than as work.
    bool mVerifying = false;
    /// Inside consumeDirty: markMaterialUsersDirty appends to THIS sync's list
    /// rather than to the document's (a shading-model switch must re-push the
    /// query flags of every node sharing the material on the same frame).
    bool mConsumingDirty = false;
    bool mVerifyEverything = false;
    /// JAH_MIRROR_TRACE=1: name every node the document reported, per sync.
    bool mTrace = false;
    /// MEASURED (8,404-node lattice, Debug + ASan, 2026-09-13): the verifier is
    /// the DOMINANT term in a still frame's mirror once the walk is gone —
    /// host.mirror 0.558 ms median, of which mirror.verify is 0.462. Most of
    /// that is re-deriving each node's parent answers from the document
    /// (isVisibleInScene + resolvedMobility, which the walk threads down for
    /// free), which is exactly the work that makes it an independent check. 32
    /// a sync is a full pass over that lattice every ~4.4 s at 60 Hz for about
    /// a quarter of a millisecond — "the screen catches up within a second or
    /// two" either way, and the differential suite is the real gate.
    unsigned mVerifierBudget = 32;
    /// How many MATERIALS the amortised verifier re-fingerprints per sync.
    /// Separate from the node budget, and much smaller, because the two costs
    /// are an order of magnitude apart: re-checking a node's latches is ~1 us,
    /// re-hashing a material's forty-odd fields and its texture map is ~15 in a
    /// Debug build. The node pass is what catches a missing mark on an object;
    /// this is what catches one on a material, more slowly — and the
    /// differential suite (JAH_MIRROR_VERIFY=full, verifyAgainstFullWalk) is
    /// what catches either one at once.
    unsigned mVerifierMaterialBudget = 8;
    /// What is left of that budget inside the current verification pass.
    /// Unbounded during verifyAgainstFullWalk: the oracle re-reads everything.
    unsigned mVerifyMaterialQuota = 0;
    /// Where the rotating verifier got to in mEntries.
    quint64 mVerifierCursor = 0;
    quint64 mDirtyNodes = 0;
    quint64 mEvictedNodes = 0;
    quint64 mVerifierVisits = 0;
    quint64 mVerifierCatches = 0;
    quint64 mVisitPushes = 0;
    /// The last global material revision this mirror folded in (§3.6): one
    /// relaxed atomic read is what a still frame pays to know that no material
    /// in the process has been written.
    quint64 mMaterialRevision = 0;
    /// Scratch for the swapped change list and the eviction list — members so
    /// that a frame's capacity is the previous frame's, never an allocation.
    std::vector<iris::SceneNode *> mDirtyScratch;
    std::vector<iris::SceneNode *> mEvictedScratch;
    /// PER-SYNC MEMO of "what did this parent resolve to" — bit 0 shown, bit 1
    /// movable. A marked node reads its parent's answers from the DOCUMENT
    /// (O(depth)) so that the order of the change list cannot matter; a
    /// thousand physics bodies under twenty groups would pay that walk a
    /// thousand times for twenty answers. Cleared at the head of every
    /// consumption, so it can never outlive the frame that filled it.
    QHash<const iris::SceneNode *, quint8> mParentState;
    /// Which document nodes draw each material. Maintained at attach and at
    /// release, and read by markChangedMaterials — a material edit has to
    /// reach the nodes that draw it without a walk of the scene.
    QHash<iris::Material *, std::vector<iris::SceneNode *>> mMaterialUsers;
    /// How many ENTRIES currently carry a refractive / distortion material and
    /// a rig — the walk-derived aggregates of §1.4, kept incrementally so that
    /// a still frame recomputes none of them.
    quint32 mRefractiveEntries = 0;
    quint32 mDistortionEntries = 0;
    /// The moving lamps' own change key, kept OUT of mGiLightSignature: a lamp
    /// that moves must re-inject its light into the voxels (owner decision O2)
    /// and must NOT arm the settle, or an animated torch would hold the
    /// stability window open for ever and pay a full re-solve every time it
    /// paused. `mGiMovableLightsMoving` is what carries the cadence when no
    /// settle is pending at all.
    quint64 mGiMovableLightSignature = 0;
    bool    mGiMovableLightsMoving = false;
    /// ...AND THE REST FRAME IT OWES (round-2 review F1). A cadence tick taken
    /// while the lamp was moving injects at ONE bounce and the coarse ray march
    /// (Scene::refreshGiLighting's `inMotion`), which is right while it moves
    /// and wrong the moment it stops — and this path arms no settle, so nothing
    /// else would ever run the full count again: a three-bounce room with a
    /// Movable lamp stayed lit at one bounce for the rest of the session. The
    /// latch keeps the cadence alive for exactly one more tick after the motion
    /// stops, and that tick is the `inMotion = false` one. A full re-solve
    /// (the settle, or an explicit refresh) supersedes it and clears it.
    bool    mGiMovableSettleOwed = false;
    quint64 mGiLightRefreshAtRestCount = 0;
    /// The play edge the soft-promotion rule is scoped to. Play STOP clears the
    /// document's soft flags (Scene::setPlaying) and the per-node warn latches
    /// here, so a second play session starts clean.
    bool mWasPlaying = false;
    // ---- Rebuild coalescing (REFLECTIONS_ADOPTION_SPEC.md §5 / P2) ----
    // A moving light ARMS a refresh instead of performing one; the expensive
    // rebuild fires when the light has held still. See applyEnvironment for the
    // argument. The constants are deliberately not exposed: they are a debounce,
    // not a preference.
    bool     mGiPendingRefresh = false;
    int      mGiStableFrames = 0;
    int      mGiFramesSinceLightOnly = 0;
    /// The value of Scene::giRefreshSerial this mirror has already acted on
    /// (P1d). An EXPLICIT refresh never waits for the stability window.
    quint64  mGiRefreshSerialSeen = 0;
    /// Frames of a still light signature before the full rebuild fires. 15 at
    /// 60 Hz is a quarter second — long enough that no drag ever crosses it,
    /// short enough that letting go feels immediate.
    ///
    /// FRAMES ONLY, AND NEVER A WALL CLOCK (COLDGI-1, 2026-09-15). This gate
    /// used to fire on `kGiStableFrames` frames OR 250 ms, whichever came
    /// first, so that a slow Debug frame rate could not stretch the wait into
    /// seconds. That made the number of GI RE-SOLVES a function of how fast
    /// the machine rendered: the same script, stepping the same frames, fired
    /// one extra full re-solve on a cold shader cache (where the first frames
    /// cost seconds) and none on a warm one. `threading.mode_pixels` caught it
    /// as two halves of one comparison running different GI histories
    /// (staleSerial 8 against 9). It is the `cameras.exposure` lesson in its
    /// second guise: a wall-clock settle measures nothing in an engine whose
    /// clock is the simulated 1/60 s frame — count frames, and the answer is
    /// the same on every machine and in every gate. The cost of dropping the
    /// clock is honest and small: on a box rendering the scene at 10 fps the
    /// settle after a drag takes 1.5 s instead of 250 ms. A picture that
    /// depends on the machine is worse than a settle that depends on the
    /// frame rate, which is what the user is watching anyway.
    static constexpr int kGiStableFrames = 15;
    /// While waiting, run the cheap light-only re-inject this often, so bounced
    /// light follows a light that is being dragged.
    static constexpr int kGiLightOnlyEveryN = 10;
    // Fog: last pushed state. Enabling/disabling fog creates or destroys the
    // scene's atmosphere and changes the shader variant, so this one is pushed on
    // change only, not every frame.
    jahshaka::engine::FogDesc mLastFog;
    bool mFogPushed = false;
};

#endif // SCENEMIRROR_H
