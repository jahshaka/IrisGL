#include "core/math/mat3.h"
#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "irisgl/mirror/scenemirror.h"

#include <cstring>
#include <algorithm>
#include <cmath>
#include <type_traits>

#include <QSet>

#include "irisgl/document/scenegraph/nodegraph.h"
#include "irisgl/document/scenegraph/scene.h"
#include "irisgl/document/scenegraph/skybake.h"
#include "irisgl/document/scenegraph/scenenode.h"
#include "irisgl/document/scenegraph/meshnode.h"
#include "irisgl/document/scenegraph/lightnode.h"
#include "irisgl/document/scenegraph/decalnode.h"
#include "irisgl/document/scenegraph/cameranode.h"
#include "irisgl/document/scenegraph/looks.h"
#include "irisgl/document/scenegraph/particlesystemnode.h"
#include "irisgl/document/assets/mesh.h"
#include "irisgl/document/assets/skeleton.h"
#include "irisgl/document/animation/animation.h"
#include "irisgl/document/animation/clipextractor.h"
#include "irisgl/document/animation/locomotion.h"   // the N-clip push's source (Stage 5)
#include "irisgl/document/assets/vertexlayout.h"
#include "irisgl/document/assets/vertexbuffer.h"     // VertexBuffer / IndexBuffer (CPU copies)
#include "irisgl/document/materials/material.h"
#include "irisgl/document/materials/pbrmaterial.h"
#include "irisgl/document/materials/defaultmaterial.h"
#include "irisgl/core/properties/property.h"
#include "irisgl/core/math/trs.h"
#include "irisgl/document/assets/livetextures.h"
#include "irisgl/document/assets/texture2d.h"
#include "irisgl/document/scenegraph/shadowmap.h"
#include <QFileInfo>
#include <functional>
#include <QtMath>

using namespace jahshaka::engine;

namespace {
inline Vec3 toVec3(const iris::Vec3 &v) { return Vec3(v.x(), v.y(), v.z()); }
inline Quat toQuat(const iris::Quat &q) { return Quat(q.x(), q.y(), q.z(), q.scalar()); }

/// The mirror's per-frame "has anything changed?" hash (deep audit 2026-09,
/// area 8 — the biggest measurable Qt cost in the hot path).
///
/// Every one of these tests used to build a QString: QTextStream for the
/// particle signature (an allocation, a locale-aware float format and a
/// heap-grown buffer per emitter per frame), operator+ chains for textures and
/// decals, ten QString::arg calls for the sky. They exist ONLY to be compared
/// with the previous frame's value, so a 64-bit FNV-1a over the same bytes is
/// the same test with no allocation at all.
///
/// Raw-byte hashing of floats is deliberate: it is a CHANGE test, not a
/// numeric comparison. (A NaN parameter therefore re-pushes every frame
/// instead of comparing equal to itself — a degenerate authoring state that
/// costs one extra engine call, never a wrong pixel.)
struct Hasher {
    quint64 h = 1469598103934665603ull;          // FNV-1a 64 offset basis
    void bytes(const void *p, size_t n) {
        const unsigned char *b = static_cast<const unsigned char *>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    }
    /// Any trivially-copyable value (floats, ints, enums, small structs).
    template <class T> Hasher &operator<<(const T &v) {
        static_assert(std::is_trivially_copyable<T>::value, "hash raw bytes only");
        bytes(&v, sizeof(T));
        return *this;
    }
    /// Strings fold in their LENGTH as well as their characters, so that
    /// ("ab","c") and ("a","bc") cannot collide across field boundaries.
    Hasher &operator<<(const QString &s) {
        const quint32 n = quint32(s.size());
        bytes(&n, sizeof n);
        bytes(s.utf16(), size_t(s.size()) * sizeof(char16_t));
        return *this;
    }
    Hasher &operator<<(const iris::Vec3 &v) { return *this << v.x() << v.y() << v.z(); }
    Hasher &operator<<(const iris::Quat &q) { return *this << q.x() << q.y() << q.z() << q.scalar(); }
};

/// FNV-1a over one float's bit pattern, a WORD at a time. Hasher::bytes runs
/// the mix per BYTE, which is right for strings and four times the work here —
/// and this is on a per-light, per-selected-mesh, per-ancestor path.
inline void mixFloat(quint64 &h, float f)
{
    quint32 bits;
    std::memcpy(&bits, &f, sizeof bits);
    h = (h ^ quint64(bits)) * 1099511628211ull;
}

constexpr quint64 kTrsBasis = 1469598103934665603ull;   // FNV-1a 64 offset basis

/// One node's own local TRS, folded into `sig`.
inline void mixLocalTrs(quint64 &sig, iris::graph::NodeHandle h)
{
    const iris::Vec3 p = iris::graph::localPos(h);
    const iris::Quat r = iris::graph::localRot(h);
    const iris::Vec3 s = iris::graph::localScale(h);
    mixFloat(sig, p.x()); mixFloat(sig, p.y()); mixFloat(sig, p.z());
    mixFloat(sig, r.x()); mixFloat(sig, r.y()); mixFloat(sig, r.z()); mixFloat(sig, r.scalar());
    mixFloat(sig, s.x()); mixFloat(sig, s.y()); mixFloat(sig, s.z());
}

/// A change-key for a node's WORLD transform, WITHOUT computing one.
///
/// `globalTransform()` is `Ogre::Node::_getFullTransformUpdated()`, and that
/// call recurses to the ROOT unconditionally, running the full transform
/// recomposition (and a listener call) at every level — every time, whether
/// anything moved or not. Three per-frame sites only ever wanted to know
/// whether the answer CHANGED (the GI light signature, the light icon's
/// billboard instance and the selection outline's shells), and paid the whole
/// recomposition per light / per selected mesh per frame to find out (audit
/// F7 / F8 / F9).
///
/// This reads the local TRS of the node and of every ancestor — plain member
/// reads, no matrix maths — and folds them into one 64-bit hash. Exactly as
/// sensitive as the world matrix: a world transform can only change if some
/// local TRS on the chain did. (A hash is a change test, not a value: the
/// collision risk is one refusal to notice a move in 2^64, against a whole
/// matrix chain per frame.)
quint64 worldTrsSignature(iris::graph::NodeHandle h)
{
    quint64 sig = kTrsBasis;
    for (; h; h = iris::graph::parentOf(h)) mixLocalTrs(sig, h);
    return sig;
}

/// worldTrsSignature with the ANCESTOR half memoised — for the one caller that
/// asks about many nodes at once (the GI light signature). A scene's lights
/// share their parent chains, so without this the same ancestors are re-read
/// once per light per frame; with it the chain is walked once per distinct
/// parent. `memo` is a caller-owned scratch buffer (a linear scan: a scene has
/// a handful of distinct light parents, and a QHash would allocate).
quint64 worldTrsSignatureMemo(iris::graph::NodeHandle h,
                              std::vector<std::pair<iris::graph::NodeHandle, quint64>> &memo)
{
    if (!h) return kTrsBasis;
    const iris::graph::NodeHandle parent = iris::graph::parentOf(h);
    quint64 sig = kTrsBasis;
    bool have = false;
    for (const auto &e : memo)
        if (e.first == parent) { sig = e.second; have = true; break; }
    if (!have) {
        sig = worldTrsSignature(parent);
        memo.emplace_back(parent, sig);
    }
    mixLocalTrs(sig, h);
    return sig;
}

}   // namespace

SceneMirror::SceneMirror(Scene *target) : mTarget(target)
{
    // The mirror is the only thing in the program that can see BOTH a document
    // node and the engine pose of its rig, so it is the mirror that hands the
    // socket resolver its pose source (CAMERAS_SPEC §5). Without one the
    // resolver still works — at the rig's bind pose — which is exactly what a
    // document-only host should get.
    mSockets.setPoseSource([this](iris::MeshNode *node, QHash<QString, iris::Mat4> &out) {
        return boneWorldTransforms(node, out);
    });
}

SceneMirror::~SceneMirror()
{
    // The engine scene may already be gone (Engine destroyed first); only touch it
    // if the caller kept the documented order. Entries are cheap to drop.
    //
    // The DOCUMENT, however, must come out of the engine's scene manager
    // (SPECS/SCENEGRAPH_SPEC.md D2: its nodes ARE that manager's nodes now).
    // Unbinding here covers the ordinary case; a caller that destroys the engine
    // scene BEFORE letting go of the mirror gets a loud warning out of
    // Scene::setGraphScene instead of a silent read-after-destroy.
    //
    // ONLY IF WE STILL OWN THE GRAPH: another mirror may have taken it since
    // (player/editor share one document — 2026-09-05). Yanking it to staging
    // from a non-owner would silently blank the owner's view; and the hook is
    // the owner's, not ours to fire or clear.
    if (mSource && mSource->graphScene() == mBoundHandle) {
        mSource->_setGraphEvacuationHook(nullptr);   // entries are dropped, not released
        mSource->setGraphScene(iris::graph::stagingScene());
    }
}

void SceneMirror::setSource(iris::ScenePtr scene)
{
    mCharacterRigs.clear();
    // THE RIDERS COME OFF THEIR BONES FIRST, and this is not tidiness: a rider's
    // Ogre parent is a TagPoint of THIS engine scene, so a rider left on one is
    // not a child of anything the DOCUMENT tree contains — and the unbinding
    // below walks that tree to take the document out of the engine's scene
    // manager. A rider skipped there is a node left behind in a scene manager
    // the caller is about to destroy, and the document node holding its handle
    // faults at its own destruction (found by sockets.render's teardown).
    releaseAllRiders();
    for (Entry &e : mEntries) releaseEntry(e);
    mEntries.clear();
    // The outgoing document leaves the engine's scene manager before anything
    // else is torn down: its nodes live IN that manager (one tree), and the
    // caller is free to destroy the engine scene the moment this returns.
    // Ownership-guarded (2026-09-05): if another mirror has taken the graph
    // since, it is not ours to move — and the hook registered on the document
    // is the owner's, not ours.
    if (mSource && mSource->graphScene() == mBoundHandle) {
        mSource->_setGraphEvacuationHook(nullptr);   // entries released above
        mSource->setGraphScene(iris::graph::stagingScene());
    }
    for (MeshId &m : mWireMeshes) { if (m) mTarget->destroyMesh(m); m = 0; }
    if (mGridNode) {                    // removeNode reparents children, so drop them explicitly
        if (mGridMinorNode) mTarget->removeNode(mGridMinorNode);
        if (mGridMajorNode) mTarget->removeNode(mGridMajorNode);
        mTarget->removeNode(mGridNode);
        mGridNode = mGridMinorNode = mGridMajorNode = 0;
    }
    if (mGridMinorMesh) { mTarget->destroyMesh(mGridMinorMesh); mGridMinorMesh = 0; }
    if (mGridMajorMesh) { mTarget->destroyMesh(mGridMajorMesh); mGridMajorMesh = 0; }
    if (mGridMinorMaterial) { mTarget->destroyMaterial(mGridMinorMaterial); mGridMinorMaterial = 0; }
    if (mGridMajorMaterial) { mTarget->destroyMaterial(mGridMajorMaterial); mGridMajorMaterial = 0; }
    mGridBuiltSpacing = -1.0f;
    // The GI volume overlay's two boxes (fix 9): same discipline as the grid.
    if (mGiVolLitNode)   { mTarget->removeNode(mGiVolLitNode);   mGiVolLitNode = 0; }
    if (mGiVolProbeNode) { mTarget->removeNode(mGiVolProbeNode); mGiVolProbeNode = 0; }
    if (mGiVolLitMesh)   { mTarget->destroyMesh(mGiVolLitMesh);   mGiVolLitMesh = 0; }
    if (mGiVolProbeMesh) { mTarget->destroyMesh(mGiVolProbeMesh); mGiVolProbeMesh = 0; }
    if (mGiVolLitMaterial)   { mTarget->destroyMaterial(mGiVolLitMaterial);   mGiVolLitMaterial = 0; }
    if (mGiVolProbeMaterial) { mTarget->destroyMaterial(mGiVolProbeMaterial); mGiVolProbeMaterial = 0; }
    mGiVolBuilt = false;
    mHighlighted.clear();
    for (HighlightShell &s : mHighlightShells) if (s.node) mTarget->removeNode(s.node);
    mHighlightShells.clear();
    if (mHighlightMaterial) { mTarget->destroyMaterial(mHighlightMaterial); mHighlightMaterial = 0; }
    for (MeshId m : mMeshes) mTarget->destroyMesh(m);
    mMeshes.clear();
    for (MaterialId m : mMaterials) mTarget->destroyMaterial(m);
    mMaterials.clear();
    for (TextureId t : mTextures) mTarget->destroyTexture(t);
    mTextures.clear();
    mLiveGenerations.clear();   // a key is here only while its engine texture is (code review 2026-09-10)
    mPbrPushed.clear();
    for (TextureId t : mIconTextures) mTarget->destroyTexture(t);
    mIconTextures.clear();
    // Decal-atlas slices are a FIXED, process-wide budget (32 slices), and this
    // map used to survive setSource: open five worlds with decals and the atlas
    // was full, after which every decal in the session silently projected
    // nothing. The atlas is refcounted, so releasing our references here is
    // enough — a slice another scene still holds stays alive.
    for (TextureId t : mDecalTextures) if (t) mTarget->destroyTexture(t);
    mDecalTextures.clear();
    mTarget->setSky(SkyDesc());   // no sky — which also clears the reflection cubemap
    for (TextureId &t : mSkyFaceTextures)  { if (t) mTarget->destroyTexture(t); t = 0; }
    for (TextureId &t : mReflFaceTextures) { if (t) mTarget->destroyTexture(t); t = 0; }
    mSkySource = SkySource();
    mSkyDesc = SkyDesc();
    clearSkyAmbient();
    mAmbientPushed = false;
    mSource = scene;
    // ...and the incoming one moves INTO it. This is the whole of the swap from
    // the mirror's side: after it, the engine reads the very nodes the user
    // edits, and the per-frame transform push (audit F1) has nothing to do.
    if (mSource) {
        mBoundHandle =
            reinterpret_cast<iris::graph::SceneHandle>(mTarget->nativeSceneManager());
        // setGraphScene fires the PREVIOUS owner's evacuation hook (another
        // mirror sharing this document — the player page's, say) before the
        // migration destroys the nodes its engine objects sit on.
        mSource->setGraphScene(mBoundHandle);
        mSource->_setGraphEvacuationHook([this] { evacuateEngineObjects(); });
    }
}

void SceneMirror::evacuateEngineObjects()
{
    // Same reason as in setSource: the document's graph is about to migrate out
    // of this scene manager, and a rider hanging off one of its TagPoints would
    // not travel with it.
    releaseAllRiders();
    // The document's graph is about to migrate out of our manager: everything
    // we attached to its nodes must go first, while those nodes still exist.
    // (The 2026-09-05 player→editor faults: particle systems and the planar
    // pass reading nodes the migration had already destroyed.)
    for (Entry &e : mEntries) releaseEntry(e);
    mEntries.clear();
    // The derived character rigs are keyed by document node; the document's
    // graph is leaving, so they are worth exactly nothing now. Same for the
    // socket riders: those keys are document nodes too.
    mCharacterRigs.clear();
    mBoneRiders.clear();
    for (HighlightShell &s : mHighlightShells)
        if (s.node) mTarget->removeNode(s.node);
    mHighlightShells.clear();
    mHighlighted.clear();
    mReclaimPending = true;
}

NodeId SceneMirror::engineNode(const iris::SceneNode *node) const
{
    if (!node) return 0;
    auto it = mEntries.constFind(node);
    return it == mEntries.constEnd() ? 0 : it->node;
}

MaterialId SceneMirror::engineMaterial(const iris::SceneNode *node) const
{
    if (!node) return 0;
    auto it = mEntries.constFind(node);
    return it == mEntries.constEnd() ? 0 : it->material;
}

/// A shading-model switch re-creates the RENDERABLES of every node using the
/// material, behind the mirror's back. Everything attachMesh sets itself
/// (visibility bits, render queue, static bounds, the reflector plane) is
/// correct on the new Item — but per-Item state the MIRROR owns is not, and
/// there is exactly one: the query flags behind `pickable`. A new Item is born
/// with Ogre's default query mask, so an unpickable object would quietly become
/// clickable again after a switch. Forgetting one push is why this is a named
/// function next to the switch and not a line inside it.
void SceneMirror::onMaterialItemsRebuilt(MaterialId material)
{
    if (!material) return;
    for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
        if (it->material == material) it->pickablePushed = -1;
}

int SceneMirror::sync()
{
    if (!mSource || !mSource->getRootNode()) return 0;
    // RE-TAKE the graph if another mirror took it while this view was hidden
    // (player and editor share one document; whichever page is visible syncs,
    // so the visible page owns). The previous owner's evacuation hook releases
    // its engine objects before the migration; our own entries were emptied
    // the same way when WE lost it, so the walk below rebuilds from scratch.
    if (mSource->graphScene() != mBoundHandle) {
        mSource->setGraphScene(mBoundHandle);
        mSource->_setGraphEvacuationHook([this] { evacuateEngineObjects(); });
    }
    // NO transform refresh. There is nothing to refresh: the document's world
    // transforms ARE Ogre's, resolved by the engine's threaded SIMD pass inside
    // the frame and, for the readers that need one between frames, on demand
    // (iris::graph::globalTransform -> Node::_getFullTransformUpdated).
    // Sockets (CAMERAS_SPEC §5) move nodes, so they resolve BEFORE the walk
    // that pushes transforms — a camera on a character's head has to be on the
    // head in the frame that renders it, not in the one after.
    resolveSockets();

    ++mSyncStamp;
    mVisited = 0;
    // The focus-smoothing dt for this walk (CAMERA_LENS_SPEC §3 P2). Zero on
    // the first sync, and capped at a tenth of a second: a stall must not let a
    // tracking camera jump its whole remaining focus travel in one frame.
    if (!mFocusClock.isValid()) { mFocusClock.start(); mFocusDt = 0.0f; }
    else mFocusDt = std::min(0.1f, float(mFocusClock.restart()) * 0.001f);
    // Per-material work is memoised for the duration of this walk (see
    // MaterialSync): every mesh node sharing a material used to pay for it.
    mMaterialSync.clear();
    // STATIC SHADOW MAPS, rule 3 (SHADOW_TOOLING_SPEC.md §4.3): "a caster
    // moved". The renderer cannot see it — the document writes transforms
    // straight into the shared scene graph — so the mirror watches the graph's
    // own transform-write counter and tells the engine when ANY transform in
    // the process changed since the last sync. One relaxed atomic load a frame.
    //
    // COARSE ON PURPOSE (v1): any write dirties every static map in the scene,
    // including a write to a helper wire or to a node the light cannot see. The
    // per-light range test is the recorded follow-up; being wrong here costs a
    // re-render, never a wrong picture. It does mean an ANIMATION or a physics
    // sim makes static maps cost exactly what dynamic ones cost — which is the
    // honest answer, since in those frames the shadows really are moving.
    if (mTarget) {
        const quint64 writes = quint64(iris::graph::transformWrites());
        if (writes != mLastTransformWrites) {
            mLastTransformWrites = writes;
            mTarget->dirtyStaticShadows();
        }
    }
    mAnyShadowCaster = false;
    mAnyRefractive = false;
    mAnyDistortion = false;
    mShadowFilter = ShadowFilter::Hard;
    mMaxShadowResolution = 0;
    // RAW children, no QList: SceneNode::children() builds a
    // QList<QSharedPointer> — a heap allocation plus an atomic refcount per
    // child — and the walk below runs over the whole document every frame.
    iris::SceneNode *root = mSource->getRootNode().data();
    const std::size_t rootChildren = iris::graph::childCount(root->graphNode());
    for (std::size_t i = 0; i < rootChildren; ++i)
        if (iris::SceneNode *c = iris::graph::ownerOf(iris::graph::childAt(root->graphNode(), i)))
            visit(c);
    removeMissing();
    // THE CACHE SWEEP, ON DEMAND. reclaimUnused builds three QSets out of every
    // entry in the scene; at 10k nodes that was 20k+ set inserts a frame to
    // conclude, almost always, that nothing had been dropped. An engine mesh /
    // material / texture can only become unreferenced when an entry is released
    // or when an entry's reference to one CHANGES — every such site arms the
    // flag, and only then does the sweep run.
    if (mReclaimPending) { reclaimUnused(); mReclaimPending = false; }
    // LIVE TEXTURES (ADDENDUM A-1). AFTER the sweep, so a texture the sweep
    // just freed is not uploaded into; before the frame is drawn, because the
    // engine's upload records into the OPEN command buffer and therefore lands
    // ahead of this frame's draws with no flush of ours.
    syncLiveTextures();
    // THE SHADER CLOCK (HLMS_ADOPTION P5), and only when something reads it.
    // The host owns the number: mShaderTimeOverride is what a deterministic
    // test or a scrubbed timeline sets; otherwise it is wall-clock seconds
    // since the first frame that needed one.
    if (mAnyCustomPiece) {
        if (mShaderTimeOverride >= 0.0f) mTarget->setShaderTime(mShaderTimeOverride);
        else {
            if (!mShaderClock.isValid()) mShaderClock.start();
            mTarget->setShaderTime(float(mShaderClock.nsecsElapsed()) * 1e-9f);
        }
    }
    // SHARING BEFORE CLIPS (AVATAR_RIG_PERF_SPEC §3.4): a follower carries no
    // clips at all, so which pieces are followers has to be settled before the
    // clip pass decides who to push.
    syncSkeletonSharing();
    syncClips();
    syncHighlight();
    syncGrid();
    // AFTER removeMissing: a rider deleted from the document is a dangling key
    // in the reconciler's map until its entry is released (see the function).
    sweepStaleRiders();
    return mVisited;
}

MeshId SceneMirror::engineMesh(iris::Mesh *mesh) const
{
    // The cache is keyed by (mesh, rig id) — one document mesh can back two
    // engine meshes when two characters resolve it to different rigs. This
    // DIAGNOSTIC answer is the first match; callers that need the mesh a
    // particular NODE is drawing read that node's entry instead.
    for (auto it = mMeshes.constBegin(); it != mMeshes.constEnd(); ++it)
        if (it.key().first == mesh) return it.value();
    return 0;
}

void SceneMirror::pushTransform(Scene *scene, NodeId node, const iris::Mat4 &t)
{
    const iris::Vec3 cx = t.column(0).toVector3D(), cy = t.column(1).toVector3D(), cz = t.column(2).toVector3D();
    const iris::Vec3 scale(cx.length(), cy.length(), cz.length());
    const iris::Vec3 pos = t.column(3).toVector3D();
    const float sx = scale.x() > 1e-8f ? scale.x() : 1.0f, sy = scale.y() > 1e-8f ? scale.y() : 1.0f, sz = scale.z() > 1e-8f ? scale.z() : 1.0f;
    float m[9] = { cx.x() / sx, cy.x() / sy, cz.x() / sz,
                   cx.y() / sx, cy.y() / sy, cz.y() / sz,
                   cx.z() / sx, cy.z() / sy, cz.z() / sz };
    const iris::Quat rot = iris::Quat::fromRotationMatrix(iris::Mat3(m));
    scene->setNodeTransform(node, Vec3(pos.x(), pos.y(), pos.z()),
                            Quat(rot.x(), rot.y(), rot.z(), rot.scalar()),
                            Vec3(scale.x(), scale.y(), scale.z()));
}

// ---- selection highlight -------------------------------------------------------

void SceneMirror::setHighlightedNodes(const QList<iris::SceneNodePtr> &nodes,
                                      const iris::SceneNodePtr &primary)
{
    mHighlighted.clear();
    for (const auto &n : nodes) if (n) mHighlighted.append(n);
    // The primary only counts when it is actually IN the list: the viewport
    // filters the World root and the built-in ground out of the highlight, and
    // a primary that was filtered away must not colour somebody else's shell.
    mHighlightPrimary.clear();
    if (primary)
        for (const auto &n : mHighlighted)
            if (n.data() == primary.data()) { mHighlightPrimary = primary; break; }
}

bool SceneMirror::isHighlighted(const iris::SceneNode *node) const
{
    if (!node) return false;
    for (const auto &n : mHighlighted) if (n.data() == node) return true;
    return false;
}

void SceneMirror::setHighlightWireframe(bool on)
{
    mHighlightWireframe = on;
}

/// RAW nodes, no QSharedPointer. This runs over the whole selected subtree
/// every frame; `sharedFromThis()` per child was one QSharedPointer
/// construction (and two atomic refcount ops) per node per frame to hand back
/// what `childAt` already returns raw — audit F9. `getMesh()` was the same
/// mistake one level down: it returns a MeshPtr BY VALUE.
void SceneMirror::collectHighlightMeshes(iris::SceneNode *node, bool primary,
                                         std::vector<HighlightTarget> &out)
{
    if (!node || !node->isVisible()) return;
    if (node->getSceneNodeType() == iris::SceneNodeType::Mesh) {
        auto meshNode = static_cast<iris::MeshNode *>(node);
        // The engine mesh THIS NODE is drawing, from its own entry — not a
        // lookup by document mesh, which since the character rig can answer with
        // a sibling character's copy of the same asset (mMeshes is keyed by
        // (mesh, rig id)).
        if (meshNode->mesh.data()) {
            const auto ent = mEntries.constFind(node);
            if (ent != mEntries.constEnd() && ent->mesh)
                out.push_back(HighlightTarget{ meshNode, ent->mesh, primary });
        }
    }
    const int n = node->childCount();
    for (int i = 0; i < n; ++i)
        if (iris::SceneNode *c = node->childAt(i))
            collectHighlightMeshes(c, primary, out);
}

void SceneMirror::syncHighlight()
{
    // Every mesh under the highlighted node, the node itself included: selecting
    // an asset's ROOT (or any group) outlines the whole asset, not just one part.
    // The scratch vector is a MEMBER: this is a per-frame walk, and a local
    // vector re-allocated its storage on every frame with a selection.
    std::vector<HighlightTarget> &targets = mHighlightTargets;
    targets.clear();
    // The PRIMARY's brighter outline (D4 b) only exists in a MULTI-selection:
    // with one node selected there is no "the others" to contrast against, and
    // making the lone selection change colour would move every pixel
    // app.selection_outline measures. One member = one colour, exactly as
    // before.
    const bool distinguishPrimary = mHighlighted.size() > 1 && !mHighlightPrimary.isNull();
    // N ROOTS: the same subtree walk, once per member of the set. Duplicates
    // cannot appear — a member whose ancestor is also selected contributes
    // meshes the ancestor already contributed — so they are dropped here rather
    // than growing two shells over one mesh (double-drawn outlines, and a
    // shell pool that never settles).
    for (const auto &highlighted : mHighlighted) {
        const size_t before = targets.size();
        const bool primary =
            distinguishPrimary && highlighted.data() == mHighlightPrimary.data();
        collectHighlightMeshes(highlighted.data(), primary, targets);
        for (size_t i = targets.size(); i > before; --i) {
            const size_t idx = i - 1;
            bool dup = false;
            for (size_t k = 0; k < before; ++k)
                if (targets[k].node == targets[idx].node) { dup = true; break; }
            if (dup) targets.erase(targets.begin() + long(idx));
        }
    }
    if (targets.empty()) {
        // ARM THE SWEEP ONLY ON A REAL TRANSITION (audit F4). This branch runs
        // on every frame with nothing selected — which is most frames — and it
        // used to set mReclaimPending unconditionally, so reclaimUnused()
        // (three QSets built out of every entry in the scene) ran EVERY frame
        // of an idle editor, exactly the per-frame sweep the flag exists to
        // prevent. A shell that was already released has released nothing.
        for (HighlightShell &s : mHighlightShells) {
            if (s.node && s.shown) { mTarget->setNodeVisible(s.node, false); s.shown = false; }
            if (s.mesh) {
                s.mesh = 0;
                // A parked shell stops riding its character's pose: the per-frame
                // copy should cost only what is actually on screen.
                if (s.skinned) { mTarget->followSkeleton(s.node, 0); s.skinned = false; s.master = 0; }
                mReclaimPending = true;
            }
        }
        return;
    }
    // The user's outline colour preference lives on the document
    // (scene->outlineColor, filled from Preferences by MainWindow::
    // updateSceneSettings — legacy reads it the same way). Fall back to the
    // historical selection yellow when the document never got one.
    const QColor pref = mSource ? mSource->outlineColor : QColor();
    const Colour kSelection = pref.isValid()
        ? Colour(float(pref.redF()), float(pref.greenF()), float(pref.blueF()))
        : Colour(1.0f, 0.85f, 0.1f);
    // THE PRIMARY'S COLOUR (D4 b). An explicit preference wins; with none, the
    // secondary colour is lifted HALFWAY TO WHITE, which is "lighter" for every
    // hue including the dark ones (QColor::lighter multiplies HSV value and
    // does nothing at all to a colour whose value is already 255 — the shipped
    // default #3498db is close enough to that to matter).
    const QColor primaryPref = mSource ? mSource->outlinePrimaryColor : QColor();
    const Colour kPrimary = [&]() -> Colour {
        if (primaryPref.isValid())
            return Colour(float(primaryPref.redF()), float(primaryPref.greenF()),
                          float(primaryPref.blueF()));
        return Colour(kSelection.r + (1.0f - kSelection.r) * 0.5f,
                      kSelection.g + (1.0f - kSelection.g) * 0.5f,
                      kSelection.b + (1.0f - kSelection.b) * 0.5f);
    }();
    // Live colour changes (preference edited with a selection active): every
    // highlight material is unlit, so one setter updates each in place. The
    // primary's derived default follows the SECONDARY colour, so a change to
    // either key has to re-push both families.
    if (pref != mHighlightColourApplied || primaryPref != mHighlightPrimaryColourApplied) {
        mHighlightColourApplied = pref;
        mHighlightPrimaryColourApplied = primaryPref;
        if (mHighlightMaterial) mTarget->setUnlitMaterial(mHighlightMaterial, kSelection);
        if (mOutlineMaterial)   mTarget->setUnlitMaterial(mOutlineMaterial, kSelection);
        if (mOutlineSkinnedMaterial) mTarget->setUnlitMaterial(mOutlineSkinnedMaterial, kSelection);
        if (mHighlightPrimaryMaterial)
            mTarget->setUnlitMaterial(mHighlightPrimaryMaterial, kPrimary);
        if (mOutlinePrimaryMaterial)
            mTarget->setUnlitMaterial(mOutlinePrimaryMaterial, kPrimary);
        if (mOutlinePrimarySkinnedMaterial)
            mTarget->setUnlitMaterial(mOutlinePrimarySkinnedMaterial, kPrimary);
    }
    // The material a target's shell wants, made on first use. Four flavours
    // (wireframe / hull) x (secondary / primary), plus the two skinned hulls,
    // and none of them is created by a scene that never asks for it.
    const auto materialFor = [&](bool primary, bool skinned) -> MaterialId {
        const Colour &colour = primary ? kPrimary : kSelection;
        if (mHighlightWireframe && !skinned) {
            MaterialId &slot = primary ? mHighlightPrimaryMaterial : mHighlightMaterial;
            if (!slot) slot = mTarget->createUnlitMaterial(colour, false, true);  // on top, wireframe
            return slot;
        }
        if (skinned) {
            MaterialId &slot = primary ? mOutlinePrimarySkinnedMaterial : mOutlineSkinnedMaterial;
            if (!slot) slot = mTarget->createOutlineMaterial(colour, true);
            return slot;
        }
        MaterialId &slot = primary ? mOutlinePrimaryMaterial : mOutlineMaterial;
        if (!slot) slot = mTarget->createOutlineMaterial(colour);
        return slot;
    };
    // One pooled shell per target mesh; extra shells from a previous (larger)
    // selection are hidden, not destroyed.
    if (mHighlightShells.size() < targets.size()) mHighlightShells.resize(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        iris::MeshNode *meshNode = targets[i].node;
        const MeshId m = targets[i].mesh;
        const bool primary = targets[i].primary;
        HighlightShell &s = mHighlightShells[i];
        if (!s.node) {
            s.node = mTarget->createNode();
            // AT BIRTH, like every other helper in this file (the light wires
            // at :706, the grid, the camera bodies): the selection shell is
            // EDITOR FURNITURE, and without the flag itemVisibilityFlags gives
            // it kVisibleBit at RQ 10 — i.e. it becomes scene geometry to the
            // planar-reflection RTT (mask kVisibleBit, RQ 0..199) and to the
            // PCC probe faces (visibility_mask 0x1, rq_last 200). The reflected
            // pass inverts vertex winding, so the back-face-only inverted hull
            // renders SOLID there: selecting the Mirror Room's panel flooded it
            // gold through the planar RTT, and the sphere through its probe
            // capture (owner report 2026-09-08).
            //
            // Marked here rather than after attachMesh because shells are
            // POOLED and re-attached to different meshes — the same trap
            // OgreParticles.cpp:200 records for the icons: one uncorrected
            // frame is one polluted capture. The main chain sets no explicit
            // visibility mask, so the user still sees the outline; the shadow
            // node masks to kVisibleBit, so the shell stops casting — which is
            // what an outline should do.
            if (s.node) mTarget->setNodeHelper(s.node, true);
        }
        if (!s.node) continue;

        // A SKINNED target needs a shell that follows the pose. The shell is a
        // second Item over the SAME mesh, so it comes out skeleton-animated —
        // but with a skeleton of its OWN, sitting at bind pose, and (until
        // 2026-09-06) an HlmsUnlit datablock, which has no skeletal path at all.
        // The result was a solid selection-coloured twin of the character
        // standing wherever its bind pose was, for as long as it animated.
        //
        // So: the Pbs-backed silhouette (the only Hlms here that skins) plus
        // followSkeleton, which copies the character's pose onto the shell's own
        // skeleton every frame — a COPY and not Ogre's instance sharing,
        // because a shared instance carries the character's WORLD transforms and
        // the shell would then render exactly on top of the character, its own
        // node (and with it the whole silhouette scale) meaning nothing.
        //
        // The wireframe highlight mode is unlit-only and therefore cannot skin;
        // a rigged target gets the hull silhouette in both modes rather than a
        // stale wireframe ghost.
        const auto ent = mEntries.constFind(meshNode);
        const bool skinned = ent != mEntries.constEnd() && ent->gpuSkinned && ent->node;
        const NodeId master = skinned ? ent->node : NodeId(0);
        const MaterialId shellMat = materialFor(primary, skinned);
        if (!shellMat) continue;
        if (s.mesh != m || s.wireframe != mHighlightWireframe || s.skinned != skinned
            || s.master != master || s.primary != primary) {
            if (mTarget->attachMesh(s.node, m, shellMat)) {
                s.mesh = m;
                s.wireframe = mHighlightWireframe;
                s.skinned = skinned;
                s.master = master;
                s.primary = primary;
                // The pairing is remembered engine-side, so a re-attach on
                // either end re-arms it by itself.
                mTarget->followSkeleton(s.node, master);   // 0 master = stop following
                mReclaimPending = true;   // the shell's previous mesh may be free
            }
        }
        // The outline is the same mesh scaled up slightly around the node's pivot:
        // only the band where the shell pokes out past the original is visible.
        // Band thickness follows the Preferences "outline width" the same way the
        // colour does (scene->outlineWidth, pushed by MainWindow): width/150 maps
        // the historical default 6 to the historical 1.04 hull; today's default 3
        // gives 1.02 — half the band. <=0 (never pushed) falls back to the default.
        //
        // ON CHANGE ONLY (audit F9). Deriving the shell's transform is a world
        // matrix (Ogre's full chain recomposition, up to the root), a scale, a
        // decomposition into TRS — three square roots and a
        // quaternion-from-matrix — and an engine node write, and the selection
        // is standing still on almost every frame it is up. The key is the
        // node's own world-transform signature (cheap: local TRS reads up the
        // chain, no matrix maths) plus the width, because the width scales the
        // shell without moving the node.
        const int width = (mSource && mSource->outlineWidth > 0) ? mSource->outlineWidth : 3;
        Hasher key;
        key << worldTrsSignature(meshNode->graphNode()) << width << mHighlightWireframe;
        if (!s.transformPushed || s.transformKey != key.h) {
            iris::Mat4 t = meshNode->getGlobalTransform();
            if (!mHighlightWireframe) t.scale(1.0f + float(width) / 150.0f);
            pushTransform(mTarget, s.node, t);
            s.transformKey = key.h;
            s.transformPushed = true;
        }
        if (!s.shown) { mTarget->setNodeVisible(s.node, true); s.shown = true; }
    }
    for (size_t i = targets.size(); i < mHighlightShells.size(); ++i) {
        HighlightShell &s = mHighlightShells[i];
        if (s.node && s.shown) { mTarget->setNodeVisible(s.node, false); s.shown = false; }
        if (s.mesh) {
            s.mesh = 0;
            if (s.skinned) { mTarget->followSkeleton(s.node, 0); s.skinned = false; s.master = 0; }
            mReclaimPending = true;
        }
    }
}

// ---- light wires ---------------------------------------------------------------

void SceneMirror::setLightWires(bool on)
{
    mLightWires = on;
}

void SceneMirror::setCameraBodies(bool on)
{
    mCameraBodies = on;
}

// ---- camera helpers (CAMERAS_SPEC D2 / §3, phase 2b) ------------------------------
//
// A scene camera has no geometry of its own, so the editor draws it: a small BODY
// (a boxy camera with a lens barrel and a viewfinder nub, pointing down the
// camera's -Z like the projection does) and its view FRUSTUM.
//
// Two things make this different from the light shapes above, and they are the
// reason it does not go through wireMeshFor():
//
//  * the geometry is DERIVED. The frustum is the document's own lens — fov,
//    aspect, near and far — so there is no fixed shape to cache per kind. Each
//    camera owns a mesh and it is rebuilt only when the signature of those
//    values (plus the selection state) changes; a camera nobody is editing costs
//    one 64-bit hash per frame.
//  * the far plane is CLIPPED for drawing. A camera's far clip defaults to 500
//    units: an honest frustum would be a pair of lines vanishing off screen and
//    would swallow the viewport. The drawn frustum stops at kFrustumDraw (or the
//    real far plane, whichever is nearer) — the same thing Ogre's own
//    Frustum::getCustomWorldSpaceCorners(out, customFarPlane) exists for, done
//    document-side because that is where the lens already lives.
//
// The lines are unlit and depth-test OFF, i.e. the on-top overlay render queue —
// which is also what keeps a camera out of its OWN picture-in-picture preview
// (chain::buildPip renders below that queue) with nothing to keep in step.

namespace {

/// How far down -Z the drawn frustum reaches, in world units, when the camera's
/// own far plane is further out. Chosen so a default 45-degree camera's frustum
/// is a readable wedge next to a human-scale object rather than a horizon-filling
/// funnel.
constexpr float kFrustumDraw = 6.0f;

quint64 hashFloat(quint64 h, float v)
{
    // Bit-exact: these values come from sliders and property writes, and a
    // tolerance here would let a lens creep without the wires ever rebuilding.
    quint32 bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "float is 4 bytes");
    std::memcpy(&bits, &v, sizeof(bits));
    return (h ^ quint64(bits)) * 1099511628211ull;
}

}   // namespace

// ---- focus tracking (CAMERA_LENS_SPEC §3, P2) ------------------------------
//
// `focusMode == Track` means "the focus distance is wherever that node is", and
// this is the only place in the program that can answer it: resolving it needs
// BOTH camera and target in world space, in the same frame, after sockets have
// moved anything that rides a bone. The mirror's walk is exactly that moment —
// which is why the spec puts it here rather than on the node (a document node
// cannot see the scene it is in) or in a verb (a verb runs when it is called,
// not per frame).
//
// It WRITES `focusDistance`, deliberately: in Track mode the field is a
// readout, the same way a follow-focus rig's marked distance is. Manual mode
// never touches it, so a user's authored (or keyframed) pull is safe.
//
// The distance is measured ALONG THE OPTICAL AXIS — the depth of the subject,
// which is what an image-plane blur is a function of — not the straight line to
// it. A target behind the camera has no focus distance and clamps to the
// minimum, rather than reporting a negative one that would make the optics
// nonsense downstream.
void SceneMirror::resolveFocusTracking(iris::CameraNode *camera)
{
    if (!camera || camera->focusMode != iris::CameraFocusMode::Track) return;
    if (camera->focusTarget.isEmpty() || !mSource) return;
    const iris::SceneNodePtr target = mSource->nodes.value(camera->focusTarget);
    // A guid that no longer resolves (the target was deleted) leaves the last
    // distance standing: freezing focus is a far better failure than snapping
    // to a minimum nobody asked for.
    if (!target) return;

    camera->update(0.0f);
    target->update(0.0f);
    const iris::Vec3 toTarget = target->getGlobalPosition() - camera->getGlobalPosition();
    // The camera looks down its own -Z (the whole document agrees on this).
    const iris::Vec3 forward = camera->getGlobalRotation().rotatedVector(iris::Vec3(0, 0, -1));
    float distance = iris::Vec3::dotProduct(toTarget, forward) + camera->focusOffset;
    distance = std::max(camera->minFocusDistance, distance);

    if (camera->smoothFocus && camera->focusSmoothingSpeed > 0.0f && mFocusDt > 0.0f)
        distance = iris::lens::smoothTowards(camera->focusDistance, distance,
                                             camera->focusSmoothingSpeed, mFocusDt);
    camera->focusDistance = distance;
}

void SceneMirror::syncCameraWires(Entry &e, iris::CameraNode *camera)
{
    using jahshaka::engine::Vec3;
    const bool wanted = mCameraBodies && camera->bodyVisible &&
                        camera != mViewCamera;   // never draws itself — see applyCamera
    if (!wanted) {
        if (e.wireNode) mTarget->setNodeVisible(e.wireNode, false);
        return;
    }
    if (!e.wireNode) {
        e.wireNode = mTarget->createNode(e.node);
        // EDITOR HELPER (REFLECTIONS_ADOPTION_SPEC.md P1b): wires, range circles
        // and light icons are things the user must see and a reflection probe
        // must never capture. Marked at CREATION so the very first frame of
        // geometry already carries kHelperBit.
        if (e.wireNode) mTarget->setNodeHelper(e.wireNode, true);
    }
    if (!e.wireNode) return;

    const bool selected = isHighlighted(static_cast<iris::SceneNode *>(camera));
    const float fov     = camera->angle > 0.0f ? camera->angle : 45.0f;
    // The authored aspect when the camera constrains it, its own field otherwise
    // — and never zero, which would make the frustum a plane.
    float aspect = camera->constrainAspect ? camera->aspectRatio : camera->aspectRatio;
    if (!(aspect > 0.01f)) aspect = 16.0f / 9.0f;
    const float nearClip = std::max(0.01f, camera->nearClip);
    const float farDraw  = std::max(nearClip + 0.05f, std::min(camera->farClip, kFrustumDraw));

    quint64 sig = 1469598103934665603ull;
    sig = hashFloat(sig, fov);
    sig = hashFloat(sig, aspect);
    sig = hashFloat(sig, nearClip);
    sig = hashFloat(sig, farDraw);
    sig = hashFloat(sig, camera->orthoSize);
    sig = (sig ^ quint64(camera->isPerspective ? 1 : 0)) * 1099511628211ull;
    sig = (sig ^ quint64(selected ? 2 : 0)) * 1099511628211ull;
    // The focus plane rides the same derived mesh, so it rides the same
    // signature: with it off, both terms are constant and the hash — and
    // therefore the geometry — is exactly what it was before this phase.
    sig = (sig ^ quint64(camera->focusPlaneVisible ? 4 : 0)) * 1099511628211ull;
    if (camera->focusPlaneVisible) sig = hashFloat(sig, camera->focusDistance);

    if (e.cameraSignature != sig || !e.cameraMesh) {
        std::vector<Vec3> pts;
        const auto line = [&pts](const Vec3 &a, const Vec3 &b) { pts.push_back(a); pts.push_back(b); };
        const auto box = [&](float hx, float hy, float z0, float z1) {
            const Vec3 c[8] = {
                Vec3(-hx, -hy, z0), Vec3(hx, -hy, z0), Vec3(hx, hy, z0), Vec3(-hx, hy, z0),
                Vec3(-hx, -hy, z1), Vec3(hx, -hy, z1), Vec3(hx, hy, z1), Vec3(-hx, hy, z1) };
            for (int i = 0; i < 4; ++i) {
                line(c[i], c[(i + 1) % 4]);
                line(c[4 + i], c[4 + (i + 1) % 4]);
                line(c[i], c[4 + i]);
            }
        };
        // THE BODY. A 0.5 x 0.36 x 0.6 case behind the origin, a short lens
        // barrel in FRONT of it down -Z (the direction the camera actually
        // looks), and a viewfinder nub on top — enough silhouette to read as a
        // camera at a glance and to aim a click at.
        box(0.25f, 0.18f, 0.30f, -0.30f);
        const float lensR = 0.13f;
        for (int i = 0; i < 12; ++i) {
            const float a0 = float(i) / 12 * 6.2831853f, a1 = float(i + 1) / 12 * 6.2831853f;
            const Vec3 f0(std::cos(a0) * lensR, std::sin(a0) * lensR, -0.30f);
            const Vec3 f1(std::cos(a1) * lensR, std::sin(a1) * lensR, -0.30f);
            const Vec3 b0(std::cos(a0) * lensR, std::sin(a0) * lensR, -0.48f);
            const Vec3 b1(std::cos(a1) * lensR, std::sin(a1) * lensR, -0.48f);
            line(f0, f1); line(b0, b1);
            if (i % 3 == 0) line(f0, b0);
        }
        box(0.09f, 0.07f, 0.18f, -0.05f);          // viewfinder nub, lifted below
        for (size_t i = pts.size() - 24; i < pts.size(); ++i) pts[i].y += 0.24f;

        // THE FRUSTUM, straight out of the document's lens.
        auto corners = [&](float z, Vec3 out[4]) {
            float hh, hw;
            if (camera->isPerspective) {
                hh = std::tan(fov * 0.5f * 3.14159265f / 180.0f) * z;
                hw = hh * aspect;
            } else {
                hh = std::max(0.01f, camera->orthoSize);
                hw = hh * aspect;
            }
            out[0] = Vec3(-hw, -hh, -z); out[1] = Vec3(hw, -hh, -z);
            out[2] = Vec3(hw,  hh, -z);  out[3] = Vec3(-hw, hh, -z);
        };
        Vec3 n[4], f[4];
        corners(nearClip, n);
        corners(farDraw, f);
        for (int i = 0; i < 4; ++i) {
            line(n[i], n[(i + 1) % 4]);
            line(f[i], f[(i + 1) % 4]);
            line(n[i], f[i]);
        }
        // An "up" tick on the far plane: without it a camera rolled 180 degrees
        // looks identical to one that is not.
        line(f[3], Vec3((f[2].x + f[3].x) * 0.5f, f[3].y * 1.25f, f[3].z));
        line(f[2], Vec3((f[2].x + f[3].x) * 0.5f, f[2].y * 1.25f, f[2].z));

        // THE FOCUS PLANE (CAMERA_LENS_SPEC §3 P2), off by default. A rectangle
        // across the frustum at the focus distance, with a cross through the
        // middle so it reads as a plane and not as another far-plane rectangle.
        // Drawn at the REAL distance — clipped only by the camera's own clip
        // planes, not by the frustum's drawing limit, because "the focus is
        // way out there" is exactly what a user with a 30 m pull needs to see.
        if (camera->focusPlaneVisible) {
            const float fz = std::max(nearClip,
                                      std::min(camera->focusDistance, camera->farClip));
            Vec3 p[4];
            corners(fz, p);
            for (int i = 0; i < 4; ++i) line(p[i], p[(i + 1) % 4]);
            const Vec3 mid((p[0].x + p[2].x) * 0.5f, (p[0].y + p[2].y) * 0.5f, p[0].z);
            line(Vec3(p[0].x, mid.y, p[0].z), Vec3(p[1].x, mid.y, p[1].z));
            line(Vec3(mid.x, p[0].y, p[0].z), Vec3(mid.x, p[2].y, p[2].z));
        }

        const jahshaka::engine::MeshId built = mTarget->createLineMesh(pts, false);
        if (built) {
            if (e.cameraMesh) mTarget->destroyMesh(e.cameraMesh);
            e.cameraMesh = built;
            e.cameraSignature = sig;
            e.wireKind = -2;                       // force the attach below
            if (!e.wireMaterial)
                e.wireMaterial = mTarget->createUnlitMaterial(jahshaka::engine::Colour(1, 1, 1), false);
            if (e.wireMaterial) mTarget->attachMesh(e.wireNode, e.cameraMesh, e.wireMaterial);
        }
    }
    if (!e.wireMaterial) return;
    // Selection reads the same way it does on a light: the helper takes the
    // highlight colour. (The mesh itself is rebuilt on the selection edge only
    // because the signature includes it — the geometry is identical; keeping it
    // in the signature is what would let a later phase draw a selected camera
    // differently without a second code path.)
    pushWireColour(e, selected ? jahshaka::engine::Colour(1.0f, 0.72f, 0.15f, 1.0f)
                               : jahshaka::engine::Colour(0.75f, 0.78f, 0.85f, 1.0f));
    // Wires live in the camera node's local space; undo the node's own scale so
    // a scaled camera node still draws a true frustum.
    const iris::Vec3 sc = camera->getLocalScale();
    mTarget->setNodeTransform(e.wireNode, jahshaka::engine::Vec3(), jahshaka::engine::Quat(),
                              jahshaka::engine::Vec3(sc.x() > 1e-6f ? 1.0f / sc.x() : 1.0f,
                                                     sc.y() > 1e-6f ? 1.0f / sc.y() : 1.0f,
                                                     sc.z() > 1e-6f ? 1.0f / sc.z() : 1.0f));
    mTarget->setNodeVisible(e.wireNode, true);
}

// ---- ground grid (EDITOR_SHORTCUTS_SPEC §3) --------------------------------------

void SceneMirror::setGrid(bool visible, float spacing, GridPlane plane)
{
    mGridVisible = visible;
    mGridPlane = plane;
    // Sanitise: the spacing is the editor's snap size; refuse degenerate values.
    mGridSpacing = std::min(std::max(spacing, 0.01f), 100.0f);
}

void SceneMirror::setGridExtent(float extent)
{
    mGridExtent = std::min(std::max(extent, 1.0f), 100000.0f);
}

void SceneMirror::setGridFloorOffset(float offsetY)
{
    // A hand's breadth either side of the floor is all this is for; a large
    // value would put the "ground grid" in the air.
    mGridFloorOffset = std::min(std::max(offsetY, -1.0f), 1.0f);
}

void SceneMirror::setGridColours(const Colour &minor, const Colour &major)
{
    if (mGridMinorColour.r == minor.r && mGridMinorColour.g == minor.g &&
        mGridMinorColour.b == minor.b && mGridMinorColour.a == minor.a &&
        mGridMajorColour.r == major.r && mGridMajorColour.g == major.g &&
        mGridMajorColour.b == major.b && mGridMajorColour.a == major.a)
        return;
    mGridMinorColour = minor;
    mGridMajorColour = major;
    // The engine has no "recolour this material" verb, so a change after the
    // grid exists means new materials on the next sync.
    mGridColoursDirty = mGridMinorMaterial != 0;
}

void SceneMirror::syncGrid()
{
    if (!mGridVisible) {
        if (mGridNode) mTarget->setNodeVisible(mGridNode, false);
        return;
    }
    if (mGridColoursDirty) {
        if (mGridMinorMesh) { mTarget->detachMesh(mGridMinorNode); }
        if (mGridMajorMesh) { mTarget->detachMesh(mGridMajorNode); }
        if (mGridMinorMaterial) { mTarget->destroyMaterial(mGridMinorMaterial); mGridMinorMaterial = 0; }
        if (mGridMajorMaterial) { mTarget->destroyMaterial(mGridMajorMaterial); mGridMajorMaterial = 0; }
        mGridMinorMaterial = mTarget->createUnlitMaterial(mGridMinorColour, true);
        mGridMajorMaterial = mTarget->createUnlitMaterial(mGridMajorColour, true);
        if (mGridMinorMesh) mTarget->attachMesh(mGridMinorNode, mGridMinorMesh, mGridMinorMaterial);
        if (mGridMajorMesh) mTarget->attachMesh(mGridMajorNode, mGridMajorMesh, mGridMajorMaterial);
        mGridColoursDirty = false;
    }
    bool freshNode = false;
    if (!mGridNode) {
        mGridNode = mTarget->createNode();
        if (!mGridNode) return;
        freshNode = true;
        mGridMinorNode = mTarget->createNode(mGridNode);
        mGridMajorNode = mTarget->createNode(mGridNode);
        // EDITOR HELPER (REFLECTIONS_ADOPTION_SPEC.md P1b). The grid used to be
        // baked into every reflection-probe capture — the single most visible
        // thing wrong with a probe reflection in an editor scene. Marked on the
        // LEAF nodes because that is where the Items hang; the helper flag is
        // per node, not inherited.
        mTarget->setNodeHelper(mGridMinorNode, true);
        mTarget->setNodeHelper(mGridMajorNode, true);
        // Unlit (never fogged), depth-tested (occluded by geometry), blended.
        mGridMinorMaterial = mTarget->createUnlitMaterial(mGridMinorColour, true);
        mGridMajorMaterial = mTarget->createUnlitMaterial(mGridMajorColour, true);
    }
    if (freshNode || mGridBuiltPlane != mGridPlane ||
        mGridBuiltFloorOffset != mGridFloorOffset) {
        // The mesh is authored in XZ; the node rotates it into the plane that
        // faces the requesting view. Floor sits at mGridFloorOffset — by
        // default a hair BELOW y=0, so geometry resting on the plane (the
        // default ground is at +1e-4) occludes it cleanly instead of
        // z-fighting; a top/bottom view flips that sign, or the ground hides
        // the grid entirely (setGridFloorOffset). The vertical planes pass
        // through the origin — they are alignment aids for the orthographic
        // views and nothing habitually coexists at exactly x=0 / z=0.
        static const float s = 0.70710678f;   // sin/cos 45°: a 90° rotation
        Vec3 pos(0, mGridFloorOffset, 0);
        Quat rot;                             // Floor: identity
        switch (mGridPlane) {
        case GridPlane::FrontXY: pos = Vec3(); rot = Quat(s, 0, 0, s); break;
        case GridPlane::SideYZ:  pos = Vec3(); rot = Quat(0, 0, s, s); break;
        case GridPlane::Floor: break;
        }
        mTarget->setNodeTransform(mGridNode, pos, rot, Vec3(1, 1, 1));
        mGridBuiltPlane = mGridPlane;
        mGridBuiltFloorOffset = mGridFloorOffset;
    }
    if (mGridBuiltSpacing != mGridSpacing || mGridBuiltExtent != mGridExtent) {
        if (mGridMinorMesh) { mTarget->detachMesh(mGridMinorNode); mTarget->destroyMesh(mGridMinorMesh); mGridMinorMesh = 0; }
        if (mGridMajorMesh) { mTarget->detachMesh(mGridMajorNode); mTarget->destroyMesh(mGridMajorMesh); mGridMajorMesh = 0; }
        const float extent = mGridExtent;                // ±extent units of floor
        int n = int(extent / mGridSpacing);              // lines each side of 0
        n = std::min(n, 1000);                           // hard cap on line count
        std::vector<Vec3> minor, major;
        for (int i = -n; i <= n; ++i) {
            const float p = float(i) * mGridSpacing;
            std::vector<Vec3> &dst = (i % 10 == 0) ? major : minor;
            dst.push_back(Vec3(p, 0, -extent)); dst.push_back(Vec3(p, 0, extent));
            dst.push_back(Vec3(-extent, 0, p)); dst.push_back(Vec3(extent, 0, p));
        }
        mGridMinorMesh = mTarget->createLineMesh(minor, false);
        mGridMajorMesh = mTarget->createLineMesh(major, false);
        if (mGridMinorMesh) mTarget->attachMesh(mGridMinorNode, mGridMinorMesh, mGridMinorMaterial);
        if (mGridMajorMesh) mTarget->attachMesh(mGridMajorNode, mGridMajorMesh, mGridMajorMaterial);
        mGridBuiltSpacing = mGridSpacing;
        mGridBuiltExtent = mGridExtent;
    }
    mTarget->setNodeVisible(mGridNode, true);
}

// ---- the GI volume overlay (LIGHTING_FIX fix 9) -----------------------------

void SceneMirror::setGiVolumeOverlay(bool visible)
{
    mGiVolumeVisible = visible;
}

namespace {
/// The 12 edges of an axis-aligned box, as line-mesh vertex pairs.
std::vector<Vec3> boxEdges(const Vec3 &mn, const Vec3 &mx)
{
    const Vec3 c[8] = {
        Vec3(mn.x, mn.y, mn.z), Vec3(mx.x, mn.y, mn.z),
        Vec3(mx.x, mn.y, mx.z), Vec3(mn.x, mn.y, mx.z),
        Vec3(mn.x, mx.y, mn.z), Vec3(mx.x, mx.y, mn.z),
        Vec3(mx.x, mx.y, mx.z), Vec3(mn.x, mx.y, mx.z) };
    static const int e[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4},
                                  {0,4},{1,5},{2,6},{3,7} };
    std::vector<Vec3> pts;
    pts.reserve(24);
    for (const auto &pair : e) { pts.push_back(c[pair[0]]); pts.push_back(c[pair[1]]); }
    return pts;
}
bool sameBox(const Vec3 &a0, const Vec3 &a1, const Vec3 &b0, const Vec3 &b1)
{
    return a0.x == b0.x && a0.y == b0.y && a0.z == b0.z &&
           a1.x == b1.x && a1.y == b1.y && a1.z == b1.z;
}
bool degenerateBox(const Vec3 &mn, const Vec3 &mx)
{
    return !(mx.x > mn.x) || !(mx.y > mn.y) || !(mx.z > mn.z);
}
}  // namespace

void SceneMirror::syncGiVolume()
{
    if (!mTarget) return;
    // Nothing to draw, and nothing to ASK: giStatus() is cheap but it is still
    // a call per frame, so the toggle short-circuits before it.
    if (!mGiVolumeVisible) {
        if (mGiVolBuilt) {
            if (mGiVolLitNode)   mTarget->setNodeVisible(mGiVolLitNode, false);
            if (mGiVolProbeNode) mTarget->setNodeVisible(mGiVolProbeNode, false);
        }
        return;
    }
    const jahshaka::engine::GiStatus st = mTarget->giStatus();
    const bool haveLit = st.mode != jahshaka::engine::GiMode::Off &&
                         !degenerateBox(st.boundsMin, st.boundsMax);
    const bool haveProbe = haveLit && !degenerateBox(st.probeRegionMin, st.probeRegionMax);

    if (!mGiVolLitNode) {
        mGiVolLitNode = mTarget->createNode();
        mGiVolProbeNode = mTarget->createNode();
        if (!mGiVolLitNode || !mGiVolProbeNode) return;
        // EDITOR HELPERS, marked at creation: these boxes describe where GI
        // happens and must never be captured BY it.
        mTarget->setNodeHelper(mGiVolLitNode, true);
        mTarget->setNodeHelper(mGiVolProbeNode, true);
        // Unlit, depth-tested, blended — the same treatment as the grid, so a
        // box behind geometry reads as behind it.
        mGiVolLitMaterial   = mTarget->createUnlitMaterial(Colour(0.35f, 0.85f, 1.0f, 0.65f), true);
        mGiVolProbeMaterial = mTarget->createUnlitMaterial(Colour(1.0f, 0.75f, 0.25f, 0.65f), true);
        mGiVolBuilt = true;
    }

    const auto rebuild = [&](NodeId node, MeshId &mesh, MaterialId material,
                             const Vec3 &mn, const Vec3 &mx,
                             Vec3 &cachedMin, Vec3 &cachedMax, bool have) {
        if (!have) { mTarget->setNodeVisible(node, false); return; }
        if (!mesh || !sameBox(mn, mx, cachedMin, cachedMax)) {
            if (mesh) { mTarget->detachMesh(node); mTarget->destroyMesh(mesh); mesh = 0; }
            mesh = mTarget->createLineMesh(boxEdges(mn, mx), false);
            if (mesh) mTarget->attachMesh(node, mesh, material);
            cachedMin = mn; cachedMax = mx;
        }
        mTarget->setNodeVisible(node, mesh != 0);
    };
    rebuild(mGiVolLitNode, mGiVolLitMesh, mGiVolLitMaterial,
            st.boundsMin, st.boundsMax, mGiVolLitMin, mGiVolLitMax, haveLit);
    rebuild(mGiVolProbeNode, mGiVolProbeMesh, mGiVolProbeMaterial,
            st.probeRegionMin, st.probeRegionMax, mGiVolProbeMin, mGiVolProbeMax, haveProbe);
}

MeshId SceneMirror::wireMeshFor(int kind)
{
    if (kind < 0 || kind > 4) return 0;
    if (mWireMeshes[kind]) return mWireMeshes[kind];
    std::vector<Vec3> pts;
    auto circle = [&](int axis, float r) {
        const int n = 24;
        for (int i = 0; i < n; ++i) {
            const float a0 = float(i) / n * 6.2831853f, a1 = float(i + 1) / n * 6.2831853f;
            const float c0 = std::cos(a0) * r, s0 = std::sin(a0) * r, c1 = std::cos(a1) * r, s1 = std::sin(a1) * r;
            if (axis == 0)      { pts.push_back(Vec3(0, c0, s0)); pts.push_back(Vec3(0, c1, s1)); }
            else if (axis == 1) { pts.push_back(Vec3(c0, 0, s0)); pts.push_back(Vec3(c1, 0, s1)); }
            else                { pts.push_back(Vec3(c0, s0, 0)); pts.push_back(Vec3(c1, s1, 0)); }
        }
    };
    if (kind == 4) {                       // decal: the projector box (a unit
                                           // cube, since the node carries the
                                           // real extents) + a tick down -Y,
                                           // the projection direction
        const float h = 0.5f;
        const Vec3 c[8] = { Vec3(-h,-h,-h), Vec3(h,-h,-h), Vec3(h,-h,h), Vec3(-h,-h,h),
                            Vec3(-h, h,-h), Vec3(h, h,-h), Vec3(h, h,h), Vec3(-h, h,h) };
        const int e[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4},
                               {0,4},{1,5},{2,6},{3,7} };
        for (int i = 0; i < 12; ++i) { pts.push_back(c[e[i][0]]); pts.push_back(c[e[i][1]]); }
        pts.push_back(Vec3(0, 0, 0)); pts.push_back(Vec3(0, -0.9f, 0));
    } else if (kind == 1) {                // point: three rings
        circle(0, 0.5f); circle(1, 0.5f); circle(2, 0.5f);
    } else if (kind == 3) {                // area: unit rectangle in XZ + a short normal tick
                                           // down -Y (the emit direction, like the arrow)
        const Vec3 c0(-0.5f, 0, -0.5f), c1(0.5f, 0, -0.5f), c2(0.5f, 0, 0.5f), c3(-0.5f, 0, 0.5f);
        pts.push_back(c0); pts.push_back(c1);
        pts.push_back(c1); pts.push_back(c2);
        pts.push_back(c2); pts.push_back(c3);
        pts.push_back(c3); pts.push_back(c0);
        pts.push_back(Vec3(0, 0, 0)); pts.push_back(Vec3(0, -0.4f, 0));
    } else {                               // directional / spot: an arrow down -Y (+ a cone for spot),
                                           // matching the light direction convention (document -Y)
        pts.push_back(Vec3(0, 0, 0)); pts.push_back(Vec3(0, -1.5f, 0));
        for (int i = 0; i < 4; ++i) {
            const float a = float(i) / 4 * 6.2831853f;
            pts.push_back(Vec3(0, -1.5f, 0)); pts.push_back(Vec3(std::cos(a) * 0.15f, -1.2f, std::sin(a) * 0.15f));
        }
        if (kind == 2) { const float r = 0.6f;   // spot cone
            for (int i = 0; i < 8; ++i) {
                const float a0 = float(i) / 8 * 6.2831853f, a1 = float(i + 1) / 8 * 6.2831853f;
                pts.push_back(Vec3(std::cos(a0) * r, -1.5f, std::sin(a0) * r)); pts.push_back(Vec3(std::cos(a1) * r, -1.5f, std::sin(a1) * r));
                if (i % 2 == 0) { pts.push_back(Vec3(0, 0, 0)); pts.push_back(Vec3(std::cos(a0) * r, -1.5f, std::sin(a0) * r)); }
            }
        }
    }
    mWireMeshes[kind] = mTarget->createLineMesh(pts, false);
    return mWireMeshes[kind];
}

void SceneMirror::pushWireColour(Entry &e, const Colour &c)
{
    if (!e.wireMaterial) return;
    if (e.wireColourPushed && e.wireColour == c) return;
    if (mTarget->setUnlitMaterial(e.wireMaterial, c)) {
        e.wireColour = c;
        e.wireColourPushed = true;
    }
}

void SceneMirror::syncLightWires(Entry &e, iris::LightNode *light)
{
    if (!mLightWires) {
        // Hides the wire lines AND the icon billboard set riding on wireNode
        // (the engine toggles a set's visibility flags with its owning node).
        if (e.wireNode && e.wireVisible != 0) { mTarget->setNodeVisible(e.wireNode, false); e.wireVisible = 0; }
        return;
    }
    int kind = 1;
    if (light->lightType == iris::LightType::Directional) kind = 0;
    else if (light->lightType == iris::LightType::Spot) kind = 2;
    else if (light->lightType == iris::LightType::Area) kind = 3;
    // Attenuation volumes only for the HIGHLIGHTED light (Unreal convention):
    // an unselected point light shows just its icon, an unselected spot light
    // just the direction arrow. The directional arrow and the area rectangle
    // (the light's physical shape, not a falloff volume) stay on for every
    // light; icons are always-on with the helpers toggle.
    const bool selected = isHighlighted(static_cast<iris::SceneNode *>(light));
    int shape = kind;
    if (!selected) {
        if (kind == 1) shape = -1;        // point: rings are the falloff volume
        else if (kind == 2) shape = 0;    // spot: keep the arrow, drop the cone
    }
    if (!e.wireNode) {
        e.wireNode = mTarget->createNode(e.node);
        // EDITOR HELPER (REFLECTIONS_ADOPTION_SPEC.md P1b): wires, range circles
        // and light icons are things the user must see and a reflection probe
        // must never capture. Marked at CREATION so the very first frame of
        // geometry already carries kHelperBit.
        if (e.wireNode) mTarget->setNodeHelper(e.wireNode, true);
    }
    if (!e.wireNode) return;
    if (shape < 0) {
        if (e.wireKind != -1) { mTarget->detachMesh(e.wireNode); e.wireKind = -1; }
        // the icon set rides this node
        if (e.wireVisible != 1) { mTarget->setNodeVisible(e.wireNode, true); e.wireVisible = 1; }
        syncLightIcon(e, light);
        return;
    }
    MeshId m = wireMeshFor(shape);
    if (!m) return;
    if (!e.wireMaterial) e.wireMaterial = mTarget->createUnlitMaterial(Colour(1, 1, 1), false);
    if (!e.wireMaterial) return;
    if (e.wireKind != shape) { if (mTarget->attachMesh(e.wireNode, m, e.wireMaterial)) e.wireKind = shape; }
    const QColor c = light->color;
    // On change only, like every other push here: setUnlitMaterial schedules a
    // const-buffer update, and a light's colour is edited by hand, not animated.
    pushWireColour(e, Colour(c.redF(), c.greenF(), c.blueF(), 1.0f));
    // Wires live in the light node's local space; undo the node's own scale, and
    // size the shape by the light's range so the wire shows the actual falloff
    // volume (the meshes are authored at ring radius 0.5, cone depth 1.5 /
    // base radius 0.6 — see wireMeshFor). Directional lights have no range.
    float rx = 1.0f, ry = 1.0f, rz = 1.0f;
    const float range = std::max(0.01f, light->distance);
    if (shape == 1) {
        rx = ry = rz = range / 0.5f;               // rings at radius = range
    } else if (shape == 2) {
        ry = range / 1.5f;                         // cone reaches down to range
        const float half = qDegreesToRadians(std::min(std::max(light->spotCutOff, 1.0f), 89.0f));
        rx = rz = range * std::tan(half) / 0.6f;   // base radius = range * tan(cutoff)
    } else if (shape == 3) {
        rx = std::max(light->rectWidth, 0.01f);    // unit rect scaled to the emitting rectangle
        rz = std::max(light->rectHeight, 0.01f);   // (width = local X, height = local Z; tick stays)
    }
    const iris::Vec3 s = light->getLocalScale();
    const Vec3 wireScale(rx * (s.x() > 1e-6f ? 1.0f / s.x() : 1.0f),
                         ry * (s.y() > 1e-6f ? 1.0f / s.y() : 1.0f),
                         rz * (s.z() > 1e-6f ? 1.0f / s.z() : 1.0f));
    // ON CHANGE ONLY (audit F7). The wire's transform is derived from the
    // light's range/cone/rect and the node's own scale — all of them
    // hand-edited values — but this ran every frame for every light in the
    // scene, and setNodeTransform is a real engine write plus a node dirty.
    Hasher wireKey;
    wireKey << wireScale.x << wireScale.y << wireScale.z;
    if (!e.wireXformPushed || e.wireXformKey != wireKey.h) {
        mTarget->setNodeTransform(e.wireNode, Vec3(), Quat(), wireScale);
        e.wireXformKey = wireKey.h;
        e.wireXformPushed = true;
    }
    if (e.wireVisible != 1) { mTarget->setNodeVisible(e.wireNode, true); e.wireVisible = 1; }
    syncLightIcon(e, light);
}

// The icon billboard: one camera-facing glyph at the light's position (sun for
// directional, bulb for point, spotlight for spot — like Unreal's sprites). It
// rides the wireNode so the light-wires toggle and node teardown govern it, but
// instance positions are world-space (the set hangs off the engine's static
// root). Engine-side only: document picking never sees it.
//
// IT IS AN OVERLAY, LIKE THE GIZMO (owner report 2026-09-08: "the light icons
// are grey and blurred"). Two independent causes, one per line below:
//   * BillboardLayer::Overlay draws the glyph in the on-top overlay pass, AFTER
//     the post chain, instead of inside the opaque pass where the tonemapper
//     turned a white icon into ~24% grey, bloom bled the scene into it and SMAA
//     smeared its edges;
//   * the icon texture asks for a MIP CHAIN: the shipped sun glyph is 640x640
//     and covers about thirty screen pixels, so a single level meant sampling
//     one texel in four hundred — sparkle, not detail.
void SceneMirror::syncLightIcon(Entry &e, iris::LightNode *light)
{
    if (!e.wireNode) return;
    // The document loads a per-light icon (mainwindow/scenereader); its source
    // path doubles as the image path. Fall back by light type.
    QString path = light->icon ? light->icon->getSource() : QString();
    if (path.isEmpty()) {
        switch (light->lightType) {
        case iris::LightType::Directional: path = QStringLiteral(":/icons/light.png"); break;    // the sun glyph
        case iris::LightType::Spot:        path = QStringLiteral(":/icons/spotlight.png"); break;
        // No bundled area glyph: a sentinel key makes iconTextureFor draw a
        // procedural rounded-rect panel (the Unreal-style rect-light sprite).
        case iris::LightType::Area:        path = QStringLiteral("jah://area-light-glyph"); break;
        default:                           path = QStringLiteral(":/icons/bulb.png"); break;
        }
    }
    if (!e.hasIcon || e.iconSignature != path) {
        if (!mTarget->createBillboardSet(e.wireNode, iconTextureFor(path), false, 1,
                                         jahshaka::engine::BillboardLayer::Overlay))
            return;
        e.hasIcon = true;
        e.iconSignature = path;
        e.iconPushed = false;        // a fresh set holds no instance yet
    }
    // ON CHANGE ONLY (audit F7). setBillboards rewrites the set's whole
    // instance buffer; the instance is one world position and a size, and a
    // light that is not being dragged has neither change. The position comes
    // from the node's world transform, so the key is the same cheap
    // local-TRS-chain signature the outline shells use — never
    // getGlobalPosition(), which is Ogre's full chain recomposition.
    Hasher key;
    key << worldTrsSignature(light->graphNode()) << light->iconSize;
    if (e.iconPushed && e.iconKey == key.h) return;
    BillboardInstance b;
    const iris::Vec3 p = light->getGlobalPosition();
    b.position = Vec3(p.x(), p.y(), p.z());
    b.size = light->iconSize > 0.0f ? light->iconSize : 0.5f;
    if (mTarget->setBillboards(e.wireNode, &b, 1)) {
        e.iconKey = key.h;
        e.iconPushed = true;
    }
}

TextureId SceneMirror::iconTextureFor(const QString &path)
{
    auto it = mIconTextures.constFind(path);
    if (it != mIconTextures.constEnd()) return it.value();
    // Qt resource or file path; the engine can't read resources, so upload the
    // pixels ourselves. Icons are forced to white glyphs (alpha kept) so every
    // icon reads the same regardless of the source image's colour.
    QImage img(path);
    if (path == QStringLiteral("jah://area-light-glyph")) {
        // Procedural white rounded-rect panel for area lights (no bundled glyph).
        img = QImage(32, 32, QImage::Format_RGBA8888);
        img.fill(Qt::transparent);
        const float r = 5.0f;                    // corner radius
        const float x0 = 4, x1 = 27, y0 = 7, y1 = 24;  // wider than tall: a panel
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                if (x < x0 || x > x1 || y < y0 || y > y1) continue;
                const float cx = std::min(std::max(float(x), x0 + r), x1 - r);
                const float cy = std::min(std::max(float(y), y0 + r), y1 - r);
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r)
                    img.setPixelColor(x, y, QColor(255, 255, 255, 255));
            }
    } else if (img.isNull()) {
        // No image (e.g. resources absent in tests): a plain white disc.
        img = QImage(32, 32, QImage::Format_RGBA8888);
        img.fill(Qt::transparent);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                if ((x - 15.5f) * (x - 15.5f) + (y - 15.5f) * (y - 15.5f) <= 14.0f * 14.0f)
                    img.setPixelColor(x, y, QColor(255, 255, 255, 255));
    }
    img = img.convertToFormat(QImage::Format_RGBA8888);
    uchar *bits = img.bits();
    const qsizetype n = img.width() * qsizetype(img.height());
    for (qsizetype i = 0; i < n; ++i) { bits[i * 4 + 0] = 255; bits[i * 4 + 1] = 255; bits[i * 4 + 2] = 255; }
    // MIPMAPPED (the second half of the 2026-09-08 icon fix): these images are
    // always seen minified — a 640x640 glyph at ~30 pixels — and the base level
    // alone aliases into a sparkling mess. The chain is built once, here.
    TextureId id = mTarget->createTexture(unsigned(img.width()), unsigned(img.height()),
                                          img.constBits(), true, /*mipmaps*/ true);
    mIconTextures.insert(path, id);   // cache failures (0) too: don't retry every frame
    return id;
}

void SceneMirror::visit(iris::SceneNode *node)
{
    if (!node) return;
    ++mVisited;

    Entry &e = mEntries[node];
    e.lastSeen = mSyncStamp;
    // ONE TREE (SPECS/SCENEGRAPH_SPEC.md D2). The engine no longer makes a node
    // for a document node — it ADOPTS the document's own, which is already an
    // Ogre scene node in this scene's manager. That single change deletes the
    // whole of audit F1: there is no transform to push (the engine reads the
    // graph the user edited), and no parent to re-push either (a reparent in
    // the hierarchy panel IS the reparent in the engine's tree).
    const void *graphNode = reinterpret_cast<void *>(node->graphNode());
    if (!e.node || e.graphNode != graphNode || e.graphEpoch != node->graphEpoch()) {
        if (e.node) {
            // The handle was rebuilt under us (a migration between scene
            // managers). Everything the engine hung off the old one is gone
            // with it; drop the entry's engine state and adopt afresh.
            releaseEntry(e);
        }
        if (!graphNode) return;
        e.node = mTarget->adoptNode(const_cast<void *>(graphNode));
        e.graphNode = graphNode;
        e.graphEpoch = node->graphEpoch();
        if (!e.node) return;
        e.visiblePushed = -1;      // force one visibility application
        e.pickablePushed = -1;     // ...and one query-flag application
        e.lightMaskEverPushed = false;   // ...and one lighting-channel application
    }

    e.docNode = node;
    // Visibility is still the DOCUMENT's flag (Ogre's setVisible walks a node's
    // attachments, so an empty node has no visibility of its own) — but it is
    // pushed on CHANGE only now, like every other signature-guarded half of
    // this walk, never unconditionally.
    const int wantVisible = node->visible ? 1 : 0;
    if (e.visiblePushed != wantVisible) {
        mTarget->setNodeVisible(e.node, node->visible);
        e.visiblePushed = wantVisible;
    }

    // Picking's broad phase is Ogre's RaySceneQuery (SCENEGRAPH_SPEC §2) and
    // its mask is tested inside the SIMD sweep, so `pickable` has to reach the
    // node's engine objects as QUERY FLAGS. Change-guarded; the document's flag
    // stays the authority and is re-checked exactly on the candidates.
    const int wantPickable = node->isPickable() ? 1 : 0;
    if (e.pickablePushed != wantPickable) {
        iris::graph::setPickable(node->graphNode(), wantPickable != 0);
        e.pickablePushed = wantPickable;
    }

    // LIGHTING CHANNELS, object side. Change-guarded like everything else in
    // this walk; the engine holds the value and re-applies it to any Item it
    // rebuilds, so this never has to be re-pushed on a material or mesh swap.
    // A LIGHT node runs through here too and that is harmless — it has no Item,
    // so the engine simply records the mask (the light's own copy goes out in
    // the LightDesc below).
    const quint32 wantLightMask = node->getLightMask();
    if (!e.lightMaskEverPushed || e.lightMaskPushed != wantLightMask) {
        mTarget->setNodeLightMask(e.node, wantLightMask);
        e.lightMaskPushed = wantLightMask;
        e.lightMaskEverPushed = true;
    }

    if (node->getSceneNodeType() == iris::SceneNodeType::Mesh) {
        auto *meshNode = static_cast<iris::MeshNode *>(node);
        // The members, not the by-value getters: `getMesh()`/`getMaterial()`/
        // `getSkeleton()` each return a QSharedPointer BY VALUE, so reading
        // them costs an atomic increment and decrement per mesh per frame for
        // three pointers that almost never change.
        iris::Mesh *mesh = meshNode->mesh.data();
        iris::Material *material = meshNode->material.data();
        // The pose authority for this node (null for unskinned meshes).
        if (e.skeleton.data() != meshNode->skeleton.data()) e.skeleton = meshNode->skeleton;
        // A MESH swap has to re-attach too. `e.meshPtr` was written here and
        // never read anywhere (deep audit 2026-09, area 5): setMesh() on a live
        // node changed the document and nothing else, which is why the mesh
        // picker in the properties panel is commented out and why the material
        // preview replaced whole nodes to change its subject.
        // A character whose piece set changed has a NEW union rig (a new id and
        // a new blend-index map), and a piece attached to the old one has to
        // re-attach onto it — the engine mesh for the new rig is a different
        // cache entry, so nothing has to be destroyed here.
        bool rigStale = false;
        if (e.gpuSkinned && e.characterHost) {
            const auto cr = mCharacterRigs.constFind(e.characterHost);
            rigStale = cr == mCharacterRigs.constEnd() || cr->epoch != e.characterEpoch;
        }
        if (mesh && (!e.hasMesh || e.materialPtr != material || e.meshPtr != mesh || rigStale)) {
            MaterialId mat = materialFor(material);
            bool attached = false;
            e.gpuSkinned = false;
            e.boneCount = 0;
            MeshId m = 0;
            if (mat && !e.skeleton.isNull()) {
                // GPU skinning: a SEPARATE entry point, because the engine has to
                // know the mesh is skinned before the renderable exists —
                // attaching first and rigging later yields a silently unskinned
                // character.
                //
                // THE CHARACTER RIG (AVATAR_RIG_PERF_SPEC §3.1) is resolved
                // first, because it decides BOTH the rig this piece binds and
                // which engine mesh backs it: a piece of a multi-piece character
                // binds the character's UNION rig with its own blend-index
                // remap, and a lone piece binds its own rig with no map at all —
                // byte for byte what every rigged node did before this program.
                iris::SkeletonPtr rigSkeleton = e.skeleton;
                QVector<unsigned short> blendToRig;
                const iris::SceneNode *host = nullptr;
                quint32 epoch = 0;
                if (const CharacterRig *cr = characterRigFor(node)) {
                    const auto map = cr->remaps.constFind(e.skeleton.data());
                    if (map != cr->remaps.constEnd()) {
                        rigSkeleton = cr->rig;
                        blendToRig = map.value();
                        host = cr->host;
                        epoch = cr->epoch;
                    }
                }
                SkeletonDesc rig;
                if (toSkeletonDesc(rigSkeleton, rig)) {
                    m = meshFor(mesh, QString::fromStdString(rig.id));
                    if (m && mTarget->attachSkinnedMesh(
                                 e.node, m, mat, rig,
                                 blendToRig.isEmpty() ? nullptr : blendToRig.constData(),
                                 size_t(blendToRig.size()))) {
                        attached = true;
                        e.gpuSkinned = true;
                        e.rigSkeleton = rigSkeleton;
                        e.blendToRig = blendToRig;
                        e.characterHost = host;
                        e.characterEpoch = epoch;
                        e.boneCount = rig.bones.size();
                        e.rigId = rig.id;
                        e.clipSignature.clear();     // force a clip re-attach
                    }
                }
            }
            // Anything the engine would not rig (an over-limit rig, a mesh whose
            // bone buffers went missing) still renders — at bind pose, unskinned.
            // One renderer, never two.
            if (!attached) {
                e.rigSkeleton.reset();
                e.blendToRig.clear();
                e.characterHost = nullptr;
                m = meshFor(mesh);
            }
            if (!attached && m && mat) attached = mTarget->attachMesh(e.node, m, mat);
            if (attached) {
                e.hasMesh = true; e.material = mat; e.materialPtr = material; e.mesh = m; e.meshPtr = mesh;
                mReclaimPending = true;   // the old mesh/material may now be unreferenced
                e.texturesPushed = false;
                e.shadingModelPushed = -1;   // a NEW engine material may be in either family
                e.pickablePushed = -1;   // a NEW Item carries the default query mask
                syncTextures(e, material);
            }
        } else if (!mesh && e.hasMesh) {
            // The document dropped the mesh (a node kept, its MeshPtr cleared).
            // Without this the engine kept drawing the old geometry forever.
            mTarget->detachMesh(e.node);
            mReclaimPending = true;
            e.hasMesh = false;
            e.mesh = 0;
            e.meshPtr = nullptr;
            e.gpuSkinned = false;
            e.boneCount = 0;
            e.rigSkeleton.reset();
            e.blendToRig.clear();
            e.characterHost = nullptr;
            e.texturesPushed = false;
            e.boundTextures.clear();
        } else if (e.hasMesh && e.material && material) {
            // Parameters may change every frame from the property panel, so the
            // mirror LOOKS every frame — but it only PUSHES on a change.
            // setPbrMaterial re-applies the whole datablock (a const-buffer
            // upload) and used to drag an unconditional flushRenderables along
            // with it through setTwoSidedLighting: the audit's per-frame Hlms
            // hash recompute for every renderable in the scene.
            const MaterialSync &ms = materialSyncFor(material);
            if (ms.hasPbr) {
                // THE SHADING-MODEL SWITCH GOES FIRST, and it is not part of
                // the parameter push (HLMS_ADOPTION P4a): the two families are
                // different backend material types, so the engine destroys the
                // material, rebuilds it in the other family from the parameters
                // and maps it already holds, and re-attaches every renderable.
                // setPbrMaterial deliberately ignores the model term, so
                // pushing params first would write them into a datablock about
                // to be thrown away.
                const int wantModel = int(ms.pbr.shadingModel);
                if (e.shadingModelPushed != wantModel) {
                    // Attempted-not-pushed: a refusal (Unlit on a rigged mesh)
                    // must not be retried every frame. The next real CHANGE
                    // tries again, exactly like the planar-reflector flag.
                    e.shadingModelPushed = wantModel;
                    if (mTarget->setShadingModel(e.material, ms.pbr.shadingModel))
                        onMaterialItemsRebuilt(e.material);
                }
                // ONE COMPARE PER MATERIAL, not per node. See PbrPush in the
                // header for what this replaced and why it mattered.
                PbrPush &push = mPbrPushed[e.material];
                if (!push.pushed || !(ms.pbr == push.params)) {
                    if (mTarget->setPbrMaterial(e.material, ms.pbr)) {
                        push.params = ms.pbr;
                        push.pushed = true;
                    }
                }
                noteRefractive(ms.pbr);
            }
            syncTextures(e, material);
        }
    }

    // Planar reflector flag (PLANAR_REFLECTIONS_SPEC.md §7). Pushed only on a
    // CHANGE: arming derives a world plane from the mesh's bounds and registers
    // the item as a PBS reflection receiver, which is not a per-frame call. The
    // engine refuses geometry that is not plate-like — a refusal is remembered
    // as "pushed" so the mirror does not retry (and re-set lastError) every
    // frame; the document keeps the user's flag either way, and the next real
    // change (a new mesh, a mode switch) tries again.
    {
        const int want = node->getPlanarReflector() ? 1 : 0;
        if (e.planarReflector != want) {
            mTarget->setNodePlanarReflector(e.node, want != 0);
            e.planarReflector = want;
        }
    }

    // "Do not let this object decide where GI happens" (P1a.2). ON CHANGE ONLY,
    // for the same reason: setNodeGiBoundsExcluded invalidates the GI caches, so
    // a per-frame push would re-voxelize the scene every frame.
    {
        const int want = node->getGiBoundsExcluded() ? 1 : 0;
        if (e.giBoundsExcluded != want) {
            mTarget->setNodeGiBoundsExcluded(e.node, want != 0);
            e.giBoundsExcluded = want;
        }
    }

    if (node->getSceneNodeType() == iris::SceneNodeType::ParticleSystem) {
        syncParticles(e, static_cast<iris::ParticleSystemNode *>(node));
    } else if (e.hasParticles) {
        // A node may stop being an emitter without being removed (the document
        // changes a node's type in place). removeParticleSystem is the explicit
        // counterpart setParticleSystem needs for exactly that.
        mTarget->removeParticleSystem(e.node);
        e.hasParticles = false;
        e.particleSignature = 0;
        e.particleTexture = 0;
        mReclaimPending = true;
    }

    if (node->getSceneNodeType() == iris::SceneNodeType::Light) {
        // The light rides on the mirrored node: position and direction follow the document.
        auto *light = static_cast<iris::LightNode *>(node);
        // ON CHANGE ONLY (audit F7). setLight is ~20 Ogre setters — type,
        // diffuse, specular, cast-shadows, power scale, an attenuation solve
        // (setAttenuationBasedOnRadius takes a square root and rewrites the
        // light's local AABB), spot range — plus two std::string compares for
        // the profile/mask paths, and it ran for every light in the scene on
        // every frame to re-push values a human edits by hand. It reads NOTHING
        // from the node's transform (the light rides the adopted node and the
        // graph carries position and direction), so skipping an unchanged push
        // cannot freeze a moving light.
        const LightDesc want = toLightDesc(light);
        // By value (LightDesc::operator==, beside the struct — every field
        // setLight reads is in it, which is what keeps a new field from
        // silently stopping at the first push).
        if (!e.lightPushed || want != e.lastLight) {
            if (mTarget->setLight(e.node, want)) {
                e.hasLight = true;
                e.lastLight = want;
                e.lightPushed = true;
            }
        }
        // The document's per-light shadow type (Hard/Soft/VerySoft) has no per-light
        // engine equivalent — the filter is global. Accumulate the strongest request;
        // applyEnvironment pushes it (iris::ShadowMapType orders None<Hard<Soft<VerySoft).
        if (light->lightType != iris::LightType::Area &&   // area lights cannot shadow
            light->shadowMap && light->shadowMap->shadowType != iris::ShadowMapType::None) {
            ShadowFilter f = ShadowFilter::Hard;
            if (light->shadowMap->shadowType == iris::ShadowMapType::Soft)          f = ShadowFilter::Soft;
            else if (light->shadowMap->shadowType == iris::ShadowMapType::VerySoft) f = ShadowFilter::VerySoft;
            if (!mAnyShadowCaster || int(f) > int(mShadowFilter)) mShadowFilter = f;
            mAnyShadowCaster = true;
            // Shadow Size is global too (one atlas): the largest request wins.
            if (light->shadowMap->resolution > 0)
                mMaxShadowResolution = std::max(mMaxShadowResolution,
                                                unsigned(light->shadowMap->resolution));
        }
        syncLightWires(e, light);
    }

    if (node->getSceneNodeType() == iris::SceneNodeType::Decal) {
        auto *decal = static_cast<iris::DecalNode *>(node);
        syncDecal(e, decal);
        syncDecalWires(e, decal);
    }

    if (node->getSceneNodeType() == iris::SceneNodeType::Camera) {
        // Phase 1 finally made CameraNode set its own type, which is what lets
        // this branch exist at all (CAMERAS_SPEC §1, the type-enum trap).
        auto *cam = static_cast<iris::CameraNode *>(node);
        resolveFocusTracking(cam);
        syncCameraWires(e, cam);
    }

    // `e` is a reference into a QHash and the recursion INSERTS entries, which
    // QHash does not keep value references stable across (read-after-destroy under
    // ASan) — so nothing below may touch `e`.
    const iris::graph::NodeHandle h = node->graphNode();
    const std::size_t n = iris::graph::childCount(h);
    for (std::size_t i = 0; i < n; ++i)
        if (iris::SceneNode *c = iris::graph::ownerOf(iris::graph::childAt(h, i)))
            visit(c);
}

// ---- particles ------------------------------------------------------------------
// PARTICLES_FX2_SPEC.md. The ENGINE simulates; this function translates the
// document's authoring parameters into one ParticleSystemDesc and pushes it —
// the same shape as syncDecal and setLight, and nothing like what used to be
// here (a per-frame rebuild of a BillboardInstance array holding every live
// particle, which the document had allocated one `new` at a time).
//
// Two mappings deserve their reasons written down:
//
//  * GRAVITY. The legacy integrator used a hard-coded GRAVITY = -50 scaled by
//    `gravityComplement` (Particle::GRAVITY, deleted with the simulator). The
//    same -50 is kept here so a scene authored against the old slider falls at
//    the same rate.
//  * DISSIPATE. The legacy shrink was `scale *= 1 - elapsed/life` applied per
//    CALL — which is why update(0) had to be a documented no-op. It becomes a
//    scale ramp over the LIFE FRACTION, which is what the slider always meant,
//    and it is now frame-rate independent by construction.

namespace {

/// Ogre's affector list is positional, and the engine's topology key pins the
/// ORDER as well as the kinds — so this must be deterministic. It is: the same
/// authoring state always produces the same affector sequence.
std::vector<ParticleAffectorDesc> affectorsFor(const iris::ParticleSystemNode *ps)
{
    std::vector<ParticleAffectorDesc> out;

    // 1. Colour over life. Authored keys win; with none, the emitter's flat
    //    colour already covers it and no affector is added at all.
    if (!ps->colourKeys.isEmpty()) {
        ParticleAffectorDesc a;
        a.kind = ParticleAffectorDesc::Kind::ColourKeys;
        a.keyCount = unsigned(std::min(ps->colourKeys.size(), qsizetype(6)));
        for (unsigned i = 0; i < a.keyCount; ++i) {
            const iris::ParticleColourKey &k = ps->colourKeys[int(i)];
            a.colourKeyTimes[i] = k.time;
            a.colourKeys[i] = Colour(k.r, k.g, k.b, k.a);
        }
        out.push_back(a);
    }

    // 2. Scale over life: authored keys, else the legacy dissipate booleans.
    std::vector<std::pair<float, float>> scaleRamp;
    if (!ps->scaleKeys.isEmpty()) {
        for (const iris::ParticleScaleKey &k : ps->scaleKeys)
            scaleRamp.emplace_back(k.time, k.scale);
    } else if (ps->dissipate) {
        if (ps->dissipateInv) scaleRamp = { {0.0f, 0.0f}, {1.0f, 1.0f} };   // grow in
        else                  scaleRamp = { {0.0f, 1.0f}, {1.0f, 0.0f} };   // shrink away
    }
    if (!scaleRamp.empty()) {
        ParticleAffectorDesc a;
        a.kind = ParticleAffectorDesc::Kind::ScaleKeys;
        a.keyCount = unsigned(std::min(scaleRamp.size(), size_t(6)));
        for (unsigned i = 0; i < a.keyCount; ++i) {
            a.scaleKeyTimes[i] = scaleRamp[i].first;
            a.scaleKeys[i] = scaleRamp[i].second;
        }
        out.push_back(a);
    }

    // 3. Spin: a random start angle (the legacy `randomRotation`) and/or a real
    //    spin speed, which the old system never had.
    if (ps->randomRotation || ps->rotationSpeedMin != 0.0f || ps->rotationSpeedMax != 0.0f) {
        ParticleAffectorDesc a;
        a.kind = ParticleAffectorDesc::Kind::Rotator;
        a.rotStart = 0.0f;
        a.rotEnd = ps->randomRotation ? 360.0f : 0.0f;
        a.rotSpeedMin = std::min(ps->rotationSpeedMin, ps->rotationSpeedMax);
        a.rotSpeedMax = std::max(ps->rotationSpeedMin, ps->rotationSpeedMax);
        out.push_back(a);
    }

    // 4. One force affector carrying gravity AND wind. The legacy constant is
    //    -50 m/s^2 scaled by gravityComplement.
    const iris::Vec3 force = iris::Vec3(0.0f, -50.0f * ps->gravityComplement, 0.0f) + ps->wind;
    if (!force.isNull()) {
        ParticleAffectorDesc a;
        a.kind = ParticleAffectorDesc::Kind::LinearForce;
        a.force = Vec3(force.x(), force.y(), force.z());
        out.push_back(a);
    }

    // 4b. ADDENDUM A-4: colour FADE (per-second deltas) and colour RAMP (an
    //     image sampled across life). Both write particle colour, as does
    //     colourKeys — so they are exclusive by construction here: keys already
    //     won at step 1, and the ramp wins over the fade. Emitted only when
    //     non-neutral, the same rule turbulence follows below.
    const bool haveFade = (ps->colourFade1.isValid() && ps->colourFade1.alpha() +
                           ps->colourFade1.red() + ps->colourFade1.green() +
                           ps->colourFade1.blue() != 0) ||
                          (ps->colourFade2.isValid() && ps->colourFade2.alpha() +
                           ps->colourFade2.red() + ps->colourFade2.green() +
                           ps->colourFade2.blue() != 0);
    if (!ps->colourRampImage.isEmpty()) {
        ParticleAffectorDesc a;
        a.kind = ParticleAffectorDesc::Kind::ColourRamp;
        a.colourRampPath = ps->colourRampImage.toStdString();
        out.push_back(a);
    } else if (haveFade) {
        // A QColor cannot carry a NEGATIVE delta, and a fade-out is exactly
        // that, so the components are read as SIGNED rates in [-1, 1] around
        // 0.5: 0 is -1/s, 128 is 0, 255 is +1/s. The panel says so on the row.
        const auto rate = [](const QColor &c) {
            return Colour(float(c.redF()) * 2.0f - 1.0f, float(c.greenF()) * 2.0f - 1.0f,
                          float(c.blueF()) * 2.0f - 1.0f, float(c.alphaF()) * 2.0f - 1.0f);
        };
        ParticleAffectorDesc a;
        a.kind = ParticleAffectorDesc::Kind::ColourFade;
        a.colourAdjust1 = ps->colourFade1.isValid() ? rate(ps->colourFade1) : Colour(0, 0, 0, 0);
        // With no second stage authored, stage 2 == stage 1: that IS the
        // plugin's plain ColourFader, which is why there is one kind not two.
        a.colourAdjust2 = ps->colourFade2.isValid() ? rate(ps->colourFade2) : a.colourAdjust1;
        a.colourSwitchAt = std::max(0.0f, ps->colourFadeSwitch);
        out.push_back(a);
    }

    // 4c. ADDENDUM A-4: a size RATE. Neutral is 0 additive / 1 multiplicative.
    if ((!ps->scaleRateMultiply && ps->scaleRate != 0.0f) ||
        (ps->scaleRateMultiply && ps->scaleRate != 1.0f && ps->scaleRate > 0.0f)) {
        ParticleAffectorDesc a;
        a.kind = ParticleAffectorDesc::Kind::ScaleRate;
        a.scaleRate = ps->scaleRate;
        a.scaleMultiply = ps->scaleRateMultiply;
        out.push_back(a);
    }

    // 5. Turbulence LAST, and only when asked for: DirectionRandomiser draws a
    //    random per particle even at randomness 0 and its own source calls it
    //    "not very SIMD-friendly", so an unused one is not free.
    if (ps->turbulence > 0.0f) {
        ParticleAffectorDesc a;
        a.kind = ParticleAffectorDesc::Kind::Turbulence;
        a.randomness = ps->turbulence;
        a.scope = 1.0f;
        out.push_back(a);
    }

    return out;
}

ParticleEmitterShape toEngineShape(iris::ParticleEmitterShape s)
{
    switch (s) {
    case iris::ParticleEmitterShape::Box:             return ParticleEmitterShape::Box;
    case iris::ParticleEmitterShape::Cylinder:        return ParticleEmitterShape::Cylinder;
    case iris::ParticleEmitterShape::Ellipsoid:       return ParticleEmitterShape::Ellipsoid;
    case iris::ParticleEmitterShape::HollowEllipsoid: return ParticleEmitterShape::HollowEllipsoid;
    case iris::ParticleEmitterShape::Ring:            return ParticleEmitterShape::Ring;
    case iris::ParticleEmitterShape::Point:           break;
    }
    return ParticleEmitterShape::Point;
}

ParticleOrientation toEngineOrientation(iris::ParticleOrientation o)
{
    switch (o) {
    case iris::ParticleOrientation::StretchedCommon:       return ParticleOrientation::OrientedCommon;
    case iris::ParticleOrientation::StretchedVelocity:     return ParticleOrientation::OrientedSelf;
    case iris::ParticleOrientation::PerpendicularCommon:   return ParticleOrientation::PerpendicularCommon;
    case iris::ParticleOrientation::PerpendicularVelocity: return ParticleOrientation::PerpendicularSelf;
    case iris::ParticleOrientation::Billboard:             break;
    }
    return ParticleOrientation::Point;
}

}  // namespace

ParticleSystemDesc SceneMirror::toParticleDesc(iris::ParticleSystemNode *ps, TextureId tex)
{
    ParticleSystemDesc d;
    // maxParticles is the document's cap; 0 means the user never set one.
    d.quota = ps->maxParticles > 0 ? unsigned(ps->maxParticles) : 1024u;
    d.texture = tex;
    d.additive = ps->useAdditive;
    d.alphaHash = ps->alphaHash;
    d.distortion = ps->distortion;
    d.orientation = toEngineOrientation(ps->orientation);

    ParticleEmitterDesc e;
    e.shape = toEngineShape(ps->shape);
    // The document's convention, unchanged since 2016: particles leave along the
    // node's +Y. The engine rotates this by the node's derived orientation, so
    // (0,1,0) reproduces it exactly — no adapter child, unlike lights.
    e.direction = Vec3(0, 1, 0);
    e.angleDegrees = ps->coneAngle;
    e.rate = std::max(0.0f, ps->particlesPerSecond);
    e.velocityMin = std::max(0.0f, ps->speed - ps->speedError);
    e.velocityMax = std::max(0.0f, ps->speed + ps->speedError);
    e.ttlMin = std::max(0.01f, ps->lifeLength - ps->lifeError);
    e.ttlMax = std::max(0.01f, ps->lifeLength + ps->lifeError);
    // ONE SIZE, no spread: PFX2 emitters carry a single fixed dimension pair and
    // there is no per-particle random initial size to map `scaleError` onto.
    // The field is still authored, serialized and shown — it just does nothing
    // to a particle's birth size any more (PARTICLES_FX2_SPEC §5).
    e.sizeWidth = e.sizeHeight = std::max(0.0f, ps->particleScale);
    e.colourStart = Colour(float(ps->emitColourStart.redF()), float(ps->emitColourStart.greenF()),
                           float(ps->emitColourStart.blueF()), float(ps->emitColourStart.alphaF()));
    e.colourEnd   = Colour(float(ps->emitColourEnd.redF()), float(ps->emitColourEnd.greenF()),
                           float(ps->emitColourEnd.blueF()), float(ps->emitColourEnd.alphaF()));
    e.extents = Vec3(ps->extents.x(), ps->extents.y(), ps->extents.z());
    e.innerExtents = Vec3(ps->innerExtents.x(), ps->innerExtents.y(), ps->innerExtents.z());
    e.duration = ps->burstDuration;
    e.repeatDelay = ps->burstRepeatDelay;
    e.startTime = ps->startDelay;
    d.emitters.push_back(e);

    d.affectors = affectorsFor(ps);
    return d;
}

void SceneMirror::syncParticles(Entry &e, iris::ParticleSystemNode *ps)
{
    if (!e.node) return;
    const QString texPath = ps->texture ? ps->texture->getSource() : QString();

    // Every authored value, folded into one 64-bit hash. A still emitter costs
    // this fold and nothing else — no engine call, no allocation, no
    // per-particle anything. (It used to be a QTextStream-built QString, which
    // is what "no allocation" above did NOT mean.)
    Hasher hs;
    hs << texPath << int(ps->shape) << int(ps->orientation)
       << ps->useAdditive << ps->alphaHash << ps->distortion << ps->randomRotation
       << ps->dissipate << ps->dissipateInv
       << ps->particlesPerSecond << ps->speed << ps->speedError
       << ps->lifeLength << ps->lifeError << ps->particleScale
       << ps->gravityComplement << ps->coneAngle << ps->turbulence
       << ps->rotationSpeedMin << ps->rotationSpeedMax
       << ps->burstDuration << ps->burstRepeatDelay << ps->startDelay
       << ps->maxParticles
       << ps->extents << ps->innerExtents << ps->wind
       << ps->emitColourStart << ps->emitColourEnd
       << quint32(ps->colourKeys.size());
    for (const iris::ParticleColourKey &k : ps->colourKeys)
        hs << k.time << k.r << k.g << k.b << k.a;
    hs << quint32(ps->scaleKeys.size());
    for (const iris::ParticleScaleKey &k : ps->scaleKeys)
        hs << k.time << k.scale;
    const quint64 sig = hs.h;

    if (e.hasParticles && e.particleSignature == sig) return;

    // Colour map -> srgb. Qt resource paths (":...") are not files the engine
    // can open; textureFor returns 0 and the quads render untextured white —
    // which is exactly the defect the 2026-09-03 save fix chased, so an emitter
    // whose texture cannot be resolved is left with no texture rather than
    // silently keeping a stale one.
    const TextureId tex = texPath.isEmpty() ? 0 : textureFor(texPath, true);
    if (mTarget->setParticleSystem(e.node, toParticleDesc(ps, tex))) {
        e.hasParticles = true;
        e.particleSignature = sig;
        e.particleTexture = tex;   // the engine's definition holds it: keep it alive
    mReclaimPending = true;
    }
}

void SceneMirror::reclaimUnused()
{
    QSet<MeshId> usedMeshes; QSet<MaterialId> usedMaterials;
    for (const Entry &e : mEntries) { if (e.mesh) usedMeshes.insert(e.mesh); if (e.material) usedMaterials.insert(e.material); }
    for (const HighlightShell &s : mHighlightShells) if (s.mesh) usedMeshes.insert(s.mesh);
    for (auto it = mMeshes.begin(); it != mMeshes.end();) {
        if (usedMeshes.contains(it.value())) { ++it; continue; }
        mTarget->destroyMesh(it.value()); it = mMeshes.erase(it);
    }
    for (auto it = mMaterials.begin(); it != mMaterials.end();) {
        if (usedMaterials.contains(it.value())) { ++it; continue; }
        mPbrPushed.remove(it.value());
        mTarget->destroyMaterial(it.value()); it = mMaterials.erase(it);
    }
    // Textures, the third cache — and the one that was never reclaimed at all
    // (deep audit 2026-09, area 5). Browsing an asset library or editing a
    // material's maps grew mTextures for the life of the process.
    //
    // Only the PBR-map cache is reclaimed here. NOT the icon glyphs (a fixed
    // handful, recreated constantly as helpers toggle), NOT the sky faces (the
    // sky signature owns their lifetime), and NOT the decal atlas (its slices
    // are a shared refcounted budget, released wholesale by setSource — a decal
    // whose node is momentarily unbound must not surrender its slice, because
    // the atlas can be full when it asks for it back).
    //
    // Safe only because engine-side destroyTexture now UNBINDS the texture from
    // any material that still holds it first: an HlmsPbsDatablock keeps a raw
    // TextureGpu*, so reclaiming without that would have introduced exactly the
    // stale-binding crash this lane was told to close before opening.
    QSet<TextureId> usedTextures;
    for (const Entry &e : mEntries) {
        for (TextureId t : e.boundTextures) usedTextures.insert(t);
        // A particle emitter's map is not a material binding, but it comes out
        // of the same cache and the engine's particle definition holds it for
        // as long as the emitter's signature stands.
        if (e.particleTexture) usedTextures.insert(e.particleTexture);
    }
    // The equirect sky samples a plain cache texture, and the engine keeps it
    // until the sky changes — mSkySource owns that lifetime, not this.
    if (mSkyTexture) usedTextures.insert(mSkyTexture);
    for (auto it = mTextures.begin(); it != mTextures.end();) {
        if (usedTextures.contains(it.value())) { ++it; continue; }
        mTarget->destroyTexture(it.value());
        // A live texture's generation record dies with its engine texture, or
        // the next bind of the same guid would be recorded as already current
        // and never receive its first upload.
        mLiveGenerations.remove(it.key());
        it = mTextures.erase(it);
    }
}

/// Everything the engine hung off one entry, released. Used both by
/// removeMissing (the node left the document) and by visit (the document's
/// handle was rebuilt by a migration, so the adopted id names a dead node).
void SceneMirror::releaseEntry(Entry &e)
{
    mReclaimPending = true;
    if (e.wireNode) mTarget->removeNode(e.wireNode);
    if (e.wireMaterial) mTarget->destroyMaterial(e.wireMaterial);
    // The camera body/frustum mesh belongs to this entry alone (it is derived
    // from that one camera's lens, so nothing else can reference it) and lives
    // outside the shared mMeshes cache reclaimUnused sweeps.
    if (e.cameraMesh) mTarget->destroyMesh(e.cameraMesh);
    // removeNode on an ADOPTED node releases the engine's attachments and
    // forgets the id; the node itself belongs to the document.
    if (e.node) mTarget->removeNode(e.node);
    e = Entry();
}

void SceneMirror::removeMissing()
{
    bool droppedRigged = false;
    for (auto it = mEntries.begin(); it != mEntries.end();) {
        if (it->lastSeen == mSyncStamp) { ++it; continue; }
        droppedRigged = droppedRigged || it->gpuSkinned;
        // A node that has left the document must leave the rider bookkeeping
        // with it: the map is keyed by the document node POINTER, and the
        // reconciler's sweep would otherwise call getParent() on a freed node.
        // (The engine frees the tag itself when the node is released.)
        mBoneRiders.remove(it.key());
        releaseEntry(*it);
        it = mEntries.erase(it);
    }
    // A character that lost a piece has a stale union cached against a host
    // node that may itself be gone. Dropping the derived rigs costs one walk
    // per character on the next skinned attach and nothing at all otherwise —
    // and the cache is only ever consulted at attach time.
    if (droppedRigged) mCharacterRigs.clear();
}

MeshId SceneMirror::meshFor(iris::Mesh *mesh, const QString &rigId)
{
    const QPair<iris::Mesh *, QString> key(mesh, rigId);
    auto it = mMeshes.constFind(key);
    if (it != mMeshes.constEnd()) return it.value();
    MeshData data;
    if (!toMeshData(mesh, data)) return 0;
    // A mesh with a skeleton AND bone vertex data is GPU-skinned: the blend
    // indices and weights ride in the vertex buffer and the pose reaches the GPU
    // as bone matrices, so the mesh is IMMUTABLE — uploaded once, ever. (It used
    // to be `dynamic`, and the mirror rewrote and re-uploaded every vertex every
    // time the pose changed: ~2.4 MB per character per frame at 50k vertices,
    // which is the wall "many avatars" hit long before the arithmetic did.)
    std::vector<float> bi, bw;
    if (mesh->hasSkeleton() && toSkinData(mesh, bi, bw) &&
        bi.size() == data.vertexCount() * 4) {
        data.blendIndices.resize(bi.size());
        for (size_t i = 0; i < bi.size(); ++i) {
            // Document indices are floats (the legacy GL shader cast them back
            // with int()); the engine wants uint8. Out-of-range means the mesh
            // and its skeleton disagree — clamp rather than write a wild index
            // into the vertex buffer; attachSkinnedMesh validates the range.
            const int v = int(bi[i]);
            data.blendIndices[i] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        data.blendWeights = bw;
    }
    MeshId id = mTarget->createMesh(data);
    if (id) mMeshes.insert(key, id);
    return id;
}

MaterialId SceneMirror::materialFor(iris::Material *material)
{
    PbrParams p;
    if (!material || !toPbrParams(material, p)) {
        // A material class the mirror cannot translate gets one shared neutral
        // material. Since HLMS_ADOPTION P4b the document holds PbrMaterials and
        // the legacy DefaultMaterial only, so this is a guard rather than a
        // path anything shipped takes.
        if (!mDefaultMaterial) {
            PbrParams d; d.albedo = Colour(0.8f, 0.8f, 0.8f); d.metalness = 0.0f; d.roughness = 0.6f;
            mDefaultMaterial = mTarget->createPbrMaterial(d);
        }
        return mDefaultMaterial;
    }
    noteRefractive(p);
    auto it = mMaterials.constFind(material);
    if (it != mMaterials.constEnd()) return it.value();
    MaterialId id = mTarget->createPbrMaterial(p);
    if (id) {
        mMaterials.insert(material, id);
        syncCustomPieces(material, id);
    }
    return id;
}

/// GENERATED SHADER PIECES (HLMS_ADOPTION P5). A graph material carries the
/// paths of the GLSL its own graph was lowered into; this is where they reach
/// the renderer.
///
/// Bound ONCE, at material creation, and deliberately not re-pushed per frame:
/// the paths are content-addressed, so "the graph changed" always means "a
/// different path", which means a different document material or a re-push
/// through the same route. Binding also flushes the material's renderables, so
/// doing it per frame would rebuild every shader every frame.
void SceneMirror::syncCustomPieces(iris::Material *material, MaterialId id)
{
    auto *pbr = dynamic_cast<iris::PbrMaterial *>(material);
    if (!pbr) return;
    if (pbr->customPiecePixel.isEmpty() && pbr->customPieceVertex.isEmpty()) return;
    if (!pbr->customPiecePixel.isEmpty())
        mTarget->setMaterialCustomPiece(id, pbr->customPiecePixel.toStdString(),
                                        CustomPieceStage::PixelPreLights);
    if (!pbr->customPieceVertex.isEmpty())
        mTarget->setMaterialCustomPiece(id, pbr->customPieceVertex.toStdString(),
                                        CustomPieceStage::VertexPreTransform);
    // One scene-wide clock, pushed from here on because a piece exists to read
    // it. A scene with no generated piece never calls setShaderTime at all —
    // which is the point: nothing about a piece-less scene changes.
    mAnyCustomPiece = true;
}

/// Refraction "Auto" (POST_CHAIN_SPEC §9.5) needs to know whether the scene HAS
/// a refractive material right now: the chain only grows its second scene pass
/// and its full-res copy while one exists, so the cost when unused is exactly
/// zero. Accumulated as materials are visited, consumed by applyEnvironment,
/// reset by sync() — the same shape as mAnyShadowCaster.
void SceneMirror::noteRefractive(const PbrParams &p)
{
    if (p.alphaMode == PbrAlphaMode::Refractive) mAnyRefractive = true;
    // DISTORTION "Auto" (POST_LOOKS_SPEC §5.3) is resolved the same way and for
    // the same reason: its target and its two passes only enter the graph while
    // the scene actually holds a distortion material.
    if (p.shadingModel == ShadingModel::Distortion) mAnyDistortion = true;
}

TextureId SceneMirror::textureFor(const QString &path, bool srgb)
{
    // KEYED BY COLOUR SPACE, not by path alone. `srgb` decides the engine
    // texture's pixel format, so a path-only key handed the FIRST caller's
    // colour space to every later one: bind one file as a base colour (sRGB)
    // and again as a roughness map (linear) and the second silently sampled
    // the sRGB texture. Latent while every slot had a fixed flag; per-material
    // workflows (the shared metallic/specular slot changes colour space with
    // the workflow) and detail layers (diffuse sRGB, normal linear, same file
    // legal in both) make it reachable. The engine's own dedup index carries
    // the same term — both caches, one fix (MATERIAL_GAPS_SPEC I-2).
    const QString key = (srgb ? QStringLiteral("s|") : QStringLiteral("l|")) + path;
    auto it = mTextures.constFind(key);
    if (it != mTextures.constEnd()) return it.value();
    // Qt resources are not files the engine can read. This test used to be
    // QFileInfo::exists() alone, whose comment claimed to cover them and did
    // not: exists() is TRUE for ":assets/…", so the path went to the engine,
    // Ogre's resource group could not find it, and the boundary recorded
    // "loadTexture: file not found: :assets/textures/default_particle.jpg"
    // into a sink nobody read. (Every fresh emitter does this: the document's
    // ParticleSystemNode ctor defaults its texture to that resource, and
    // SceneEditService only copies the real file into the project a moment
    // later.) Found the day the error pump landed — STABILITY_PROGRAM_SPEC
    // Lane 1, which is what it is for.
    if (path.startsWith(QLatin1Char(':'))) return 0;
    // A LIVE TEXTURE (ADDENDUM A-1) has no file behind it: its pixels live in
    // the document and are re-uploaded by syncLiveTextures whenever the
    // producer bumps the generation. Born through createTexture rather than
    // loadTexture because only createTexture-born ids may be written to — the
    // engine refuses updateTexture on a file-loaded (pooled) texture by name.
    if (iris::LiveTextures::isLiveRef(path)) {
        iris::Texture2DPtr live = iris::LiveTextures::find(path);
        if (!live || !live->isLive()) return 0;
        const QImage &img = live->liveImage();
        if (img.isNull()) return 0;
        TextureId live_id = mTarget->createTexture(unsigned(img.width()), unsigned(img.height()),
                                                   img.constBits(), srgb, live->liveMipmaps());
        if (live_id) {
            mTextures.insert(key, live_id);
            mLiveGenerations.insert(key, live->liveGeneration());
        }
        return live_id;
    }
    if (!QFileInfo::exists(path)) return 0;
    TextureId id = mTarget->loadTexture(path.toStdString(), srgb);
    if (id) mTextures.insert(key, id);
    return id;
}

// ONE UPLOAD PER CHANGED GENERATION, and none at all for a still image
// (ADDENDUM A-1). The document side of a live texture is a counter: a producer
// (a script writing pixels, a video decoder handing over a frame) replaces the
// image and bumps it, and this is the only place that turns that into an
// engine call. Runs over the LIVE entries of the texture cache, which is at
// most a handful of textures in any real scene.
void SceneMirror::syncLiveTextures()
{
    if (!mTarget || mLiveGenerations.isEmpty()) return;
    for (auto it = mLiveGenerations.begin(); it != mLiveGenerations.end(); ++it) {
        // The key is "<colour space>|live://<guid>" — the cache's key, so the
        // same live texture bound in both colour spaces is two engine textures
        // and both are kept current.
        const QString path = it.key().mid(2);
        iris::Texture2DPtr live = iris::LiveTextures::find(path);
        if (!live || !live->isLive()) continue;
        const quint64 gen = live->liveGeneration();
        if (gen == it.value()) continue;
        const auto tex = mTextures.constFind(it.key());
        if (tex == mTextures.constEnd()) continue;
        const QImage &img = live->liveImage();
        if (img.isNull()) continue;
        // A REFUSED write leaves the recorded generation alone deliberately:
        // the pixels the engine holds are still the ones from the generation
        // recorded here, and pretending otherwise would hide a real mismatch
        // behind a number. (updateTexture only refuses a size change, which
        // writeLive already refuses on the document side.)
        if (mTarget->updateTexture(tex.value(), unsigned(img.width()),
                                   unsigned(img.height()), img.constBits()))
            it.value() = gen;
    }
}

/// EVERYTHING THAT DEPENDS ONLY ON THE MATERIAL, computed once per material per
/// sync instead of once per mesh per frame (see MaterialSync in the header).
///
/// The two halves used to sit inline in visit(): `toPbrParams` runs two
/// dynamic_casts and, for a shader-graph material, a scan of every property
/// with a QVariant read and a QString compare each; the texture resolve did
/// seven QHash lookups whose keys were built from `const char *` (one QString
/// construction per lookup) plus a QVector of binds and a hash over their
/// paths. A lattice of 8000 cubes sharing ONE material paid all of that 8000
/// times a frame, and the mirror's walk was ~90% of the idle tick because of it.
const SceneMirror::MaterialSync &SceneMirror::materialSyncFor(iris::Material *material)
{
    auto it = mMaterialSync.find(material);
    if (it != mMaterialSync.end()) return it.value();

    MaterialSync ms;
    ms.hasPbr = toPbrParams(material, ms.pbr);

    // Document slot name -> engine slot. PbrMaterial and DefaultMaterial naming.
    // There is no occlusion entry because there is no occlusion ROW any more
    // (HLMS_ADOPTION P2): the engine has no ambient-occlusion slot, so the
    // document stopped pretending to have one. An old file's "u_occlusionMap"
    // simply matches nothing here, which is what tolerance looks like.
    //
    // DETAIL LAYERS (MATERIAL_GAPS_SPEC GAP 2) join it with the colour-space
    // rule that makes I-2's cache-key fix load-bearing: a detail DIFFUSE map is
    // an sRGB colour and a detail NORMAL map is linear data, and nothing stops
    // a user binding the same file to both.
    struct Slot { QLatin1StringView name; PbrTextureSlot slot; bool srgb; };
    static const Slot kSlots[] = {
        { QLatin1StringView("u_baseColorMap"),  PbrTextureSlot::Albedo,    true  },
        { QLatin1StringView("u_diffuseTexture"), PbrTextureSlot::Albedo,   true  },
        { QLatin1StringView("u_normalMap"),     PbrTextureSlot::Normal,    false },
        { QLatin1StringView("u_normalTexture"), PbrTextureSlot::Normal,    false },
        { QLatin1StringView("u_metallicMap"),   PbrTextureSlot::Metalness, false },
        { QLatin1StringView("u_roughnessMap"),  PbrTextureSlot::Roughness, false },
        { QLatin1StringView("u_emissiveMap"),   PbrTextureSlot::Emissive,  true  },
        { QLatin1StringView("u_detail0Map"),       PbrTextureSlot::Detail0,      true  },
        { QLatin1StringView("u_detail1Map"),       PbrTextureSlot::Detail1,      true  },
        { QLatin1StringView("u_detail0NormalMap"), PbrTextureSlot::Detail0Nm,    false },
        { QLatin1StringView("u_detail1NormalMap"), PbrTextureSlot::Detail1Nm,    false },
        // The weight MASK is data (per-channel scalars), never a colour.
        { QLatin1StringView("u_detailWeightMap"),  PbrTextureSlot::DetailWeight, false },
        // THE REFLECTION CUBEMAP OVERRIDE (ADDENDUM A-5). An sRGB colour like
        // the sky it replaces; the bind takes the cubemap route below rather
        // than textureFor, because the slot holds a cube and not a 2D image.
        { QLatin1StringView("u_reflectionMap"),    PbrTextureSlot::Reflection,   true  },
    };
    static_assert(int(iris::PbrMaterial::kDetailLayers) == int(kDetailLayerCount),
                  "the document and the engine boundary must agree on how many "
                  "detail layers exist — this table is written out per layer");
    // The SHARED metallic/specular unit changes COLOUR SPACE with the workflow
    // (MATERIAL_GAPS_SPEC I-4): metalness is linear data, a specular map is an
    // sRGB colour, and PBSM_METALLIC/PBSM_SPECULAR are one renderer texture
    // unit. sharedMapIsSrgb() is the one table both the panel and this read.
    // Both texture caches now key on the flag (I-2), so the same file bound
    // here and as a linear map elsewhere is two engine textures, correctly.
    bool sharedSrgb = false;
    if (auto *pbrMat = dynamic_cast<iris::PbrMaterial *>(material))
        sharedSrgb = iris::PbrMaterial::sharedMapIsSrgb(pbrMat->workflow);

    // Resolve every candidate path: the textures map (Texture2D::source), then
    // shader-graph texture properties (a file path in the property value).
    for (const Slot &sl : kSlots) {
        auto tit = material->textures.constFind(sl.name);
        if (tit != material->textures.constEnd() && tit.value() && !tit.value()->source.isEmpty())
            ms.binds.push_back({ sl.slot,
                                 tit.value()->source,
                                 sl.slot == PbrTextureSlot::Metalness ? sharedSrgb : sl.srgb });
    }
    // (The CustomMaterial branch that scraped texture PATHS out of a shader
    // material's Property rows died with the class itself, HLMS_ADOPTION P4b.
    // Every material the document can hold is a PbrMaterial now, and a
    // PbrMaterial's maps arrive through `textures` above — the reader converts
    // a legacy `diffuseTexture` value into a real baseColorMap at load, which
    // is where that translation belongs.)
    // Which slot gets which file, as one hash: the whole job of this number is
    // to let a mesh conclude "unchanged" in one comparison.
    Hasher hs;
    hs << quint32(ms.binds.size());
    // The COLOUR SPACE is part of the signature: a workflow switch rebinds the
    // same file in the other space, and without the flag here the entry would
    // conclude "unchanged" and keep sampling the old one.
    for (const TextureBind &b : ms.binds) hs << int(b.slot) << b.path << quint32(b.srgb ? 1 : 0);
    ms.textureSignature = hs.h;

    return *mMaterialSync.insert(material, ms);
}

void SceneMirror::syncTextures(Entry &e, iris::Material *material)
{
    if (!material || !e.material || e.material == mDefaultMaterial) return;
    const MaterialSync &ms = materialSyncFor(material);
    const std::vector<TextureBind> &binds = ms.binds;
    const quint64 signature = ms.textureSignature;
    if (e.texturesPushed && signature == e.textureSignature) return;
    e.textureSignature = signature;
    e.texturesPushed = true;
    mReclaimPending = true;
    // Sized from the SLOT ENUM, not from a literal 5 — the detail slots
    // (GAP 2) made a hardcoded count a silent truncation.
    constexpr int kSlotCount = int(PbrTextureSlot::Count);
    bool bound[kSlotCount] = {};
    TextureId boundIds[kSlotCount] = {};
    for (const TextureBind &b : binds) {
        if (bound[int(b.slot)]) continue;
        TextureId t = b.slot == PbrTextureSlot::Reflection ? reflectionCubeFor(b.path)
                                                           : textureFor(b.path, b.srgb);
        if (t && mTarget->setPbrTexture(e.material, b.slot, t)) {
            bound[int(b.slot)] = true;
            boundIds[int(b.slot)] = t;
        }
    }
    for (int i = 0; i < kSlotCount; ++i)
        if (!bound[i]) mTarget->setPbrTexture(e.material, PbrTextureSlot(i), 0);
    // What reclaimUnused needs: the IDS, not the paths. A world switch or a
    // material edit that drops a map leaves the engine texture referenced by
    // nobody, and before this the mirror simply never freed one.
    e.boundTextures.assign(boundIds, boundIds + kSlotCount);
    e.boundTextures.erase(std::remove(e.boundTextures.begin(), e.boundTextures.end(), TextureId(0)),
                          e.boundTextures.end());
}

bool SceneMirror::toPbrParams(iris::Material *material, PbrParams &out)
{
    if (!material) return false;
    if (auto *pbr = dynamic_cast<iris::PbrMaterial *>(material)) {
        const QColor c = pbr->baseColor;
        const float f = pbr->baseColorFactor;
        out.albedo    = Colour(c.redF() * f, c.greenF() * f, c.blueF() * f, 1.0f);
        out.metalness = pbr->metallicFactor;
        // The document's roughness remap bounds apply per-texel to a sampled map;
        // the engine has no such remap, so approximate by clamping the scalar
        // factor into the (order-normalised) bounds.
        const float lo = std::min(pbr->roughnessLowerBound, pbr->roughnessUpperBound);
        const float hi = std::max(pbr->roughnessLowerBound, pbr->roughnessUpperBound);
        out.roughness = std::max(lo, std::min(pbr->roughnessFactor, hi));
        const QColor e = pbr->emissiveColor;
        out.emissive  = Colour(e.redF() * pbr->emissiveIntensity, e.greenF() * pbr->emissiveIntensity,
                               e.blueF() * pbr->emissiveIntensity, 1.0f);
        switch (pbr->alphaMode) {
        case 1:  out.alphaMode = PbrAlphaMode::Cutout;   break;
        case 2:  out.alphaMode = PbrAlphaMode::Blend;    break;
        case 3:  out.alphaMode = PbrAlphaMode::Glass;    break;  // fades diffuse, keeps reflections
        case 4:  out.alphaMode = PbrAlphaMode::Additive; break;  // Src + Dest (Unreal Additive)
        case 5:  out.alphaMode = PbrAlphaMode::Modulate; break;  // Src × Dest (Unreal Modulate)
        case 6:  out.alphaMode = PbrAlphaMode::Refractive; break; // glass that bends the background
        default: out.alphaMode = PbrAlphaMode::Opaque;   break;
        }
        out.refractionStrength = pbr->refractionStrength;
        out.alpha           = pbr->alpha;
        out.alphaCutoff     = pbr->alphaCutoff;
        out.normalMapWeight = pbr->normalFactor;
        out.uvScale[0]      = pbr->textureScale;   // U
        out.uvScale[1]      = pbr->textureScaleV;
        out.uvOffset[0]     = pbr->textureOffsetU;
        out.uvOffset[1]     = pbr->textureOffsetV;
        out.uvRotation      = pbr->textureRotation;
        out.twoSided        = pbr->renderStates.rasterState.cullMode == iris::CullMode::None;
        // HLMS_ADOPTION P1. The BRDF crosses as a NAME, never as the document's
        // index and never as the renderer's enum value: the index is a document
        // convention and the enum is a renderer bitfield, and the boundary
        // should carry neither. PbrMaterial::brdfEngineName is the one table.
        // HLMS_ADOPTION P4a. The model term rides PbrParams so the change-guard
        // NOTICES a switch; applying it is a separate, atomic engine call (see
        // the visit() branch) because the two families are different backend
        // material types and the switch re-attaches every renderable.
        out.shadingModel = pbr->shadingModel == 2 ? ShadingModel::Distortion
                         : pbr->shadingModel == 1 ? ShadingModel::Unlit
                                                  : ShadingModel::Lit;
        out.brdf               = iris::PbrMaterial::brdfEngineName(pbr->brdf).toStdString();
        out.clearCoat          = pbr->clearCoat;
        out.clearCoatRoughness = pbr->clearCoatRoughness;
        out.receiveShadows     = pbr->receiveShadows;
        out.emissiveAsLightmap = pbr->emissiveAsLightmap;
        // MATERIAL_GAPS_SPEC GAP 1. The workflow crosses as the engine's own
        // enum (a three-value boundary vocabulary, not the document's index and
        // not the renderer's), and every fresnel value crosses UNCONDITIONALLY
        // even on a metallic material: the engine's applyPbr is the one place
        // that decides which of metalness/fresnel is legal to write, because
        // they are the same float in the datablock (I-1). Sending them all and
        // branching once, there, is what keeps that rule in ONE place.
        out.workflow = pbr->workflow == 1 ? PbrParams::Workflow::Specular
                     : pbr->workflow == 2 ? PbrParams::Workflow::SpecularAsFresnel
                                          : PbrParams::Workflow::Metallic;
        const QColor sc = pbr->specularColor;
        out.specularColour = Colour(sc.redF(), sc.greenF(), sc.blueF(), 1.0f);
        out.ior = pbr->ior;
        const QColor fc = pbr->fresnelColor;
        out.fresnelColour = Colour(fc.redF(), fc.greenF(), fc.blueF(), 1.0f);
        out.useFresnelColour = pbr->useFresnelColor;
        out.separateFresnel  = pbr->separateFresnel;
        // ADDENDUM A-2: the sampler state. Both defaults are what every map was
        // hard-coded to, so an unauthored material's samplers are unchanged.
        out.anisotropy = pbr->anisotropy;
        {
            // Document map ROW -> engine slot, in one place. The row names are
            // the vocabulary (PbrMaterial::mapRowNames) and this is the only
            // translation of them into slots.
            static const struct { const char *row; PbrTextureSlot slot; } kAddrRows[] = {
                { "baseColorMap",      PbrTextureSlot::Albedo },
                { "normalMap",         PbrTextureSlot::Normal },
                { "metallicMap",       PbrTextureSlot::Metalness },
                { "roughnessMap",      PbrTextureSlot::Roughness },
                { "emissiveMap",       PbrTextureSlot::Emissive },
                { "detail0Map",        PbrTextureSlot::Detail0 },
                { "detail1Map",        PbrTextureSlot::Detail1 },
                { "detail0NormalMap",  PbrTextureSlot::Detail0Nm },
                { "detail1NormalMap",  PbrTextureSlot::Detail1Nm },
                { "detailWeightMap",   PbrTextureSlot::DetailWeight },
            };
            for (const auto &r : kAddrRows) {
                const int mode = pbr->addressFor(QLatin1String(r.row));
                out.address[size_t(r.slot)] =
                    mode == 1 ? PbrParams::AddressMode::Clamp
                  : mode == 2 ? PbrParams::AddressMode::Mirror
                  : mode == 3 ? PbrParams::AddressMode::Border
                              : PbrParams::AddressMode::Wrap;
            }
        }
        // MATERIAL_GAPS_SPEC GAP 2: the detail layers' SCALARS (their maps ride
        // the slot table above). Pushed unconditionally — every default here is
        // the renderer's own no-op value, at which it sets no shader property
        // for the layer at all.
        for (int i = 0; i < iris::PbrMaterial::kDetailLayers; ++i) {
            const auto &src = pbr->detail[i];
            auto &dst = out.detail[i];
            dst.blend        = unsigned(std::max(0, src.blend));
            dst.offsetU      = src.offsetU;
            dst.offsetV      = src.offsetV;
            dst.scaleU       = src.scaleU;
            dst.scaleV       = src.scaleV;
            dst.weight       = src.weight;
            dst.normalWeight = src.normalWeight;
        }
        return true;
    }
    // (The CustomMaterial branch that scraped diffuseColor/shininess/... out of
    // a shader material's Property rows is GONE with the class, HLMS_ADOPTION
    // P4b. Its conversion was not lost: it moved to LOAD time, in
    // src/io/builtinmaterials.cpp, where it runs once and produces a real
    // PbrMaterial instead of running per material per frame and producing an
    // approximation the panel could not show.)
    if (auto *def = dynamic_cast<iris::DefaultMaterial *>(material)) {
        // Legacy Blinn-Phong material: diffuse -> albedo, shininess -> roughness.
        const QColor c = def->getDiffuseColor();
        out.albedo    = Colour(c.redF(), c.greenF(), c.blueF(), 1.0f);
        out.metalness = 0.0f;
        const float shin = std::max(0.0f, std::min(def->getShininess(), 128.0f));
        out.roughness = 1.0f - std::sqrt(shin / 128.0f) * 0.9f;
        out.emissive  = Colour(0, 0, 0);
        // The legacy material has one uniform scale and no offset/rotation.
        out.uvScale[0] = out.uvScale[1] = def->getTextureScale();
        return true;
    }
    return false;
}

LightDesc SceneMirror::toLightDesc(iris::LightNode *light)
{
    LightDesc d;
    switch (light->lightType) {
    case iris::LightType::Directional: d.type = LightType::Directional; break;
    case iris::LightType::Spot:        d.type = LightType::Spot; break;
    case iris::LightType::Area:        d.type = LightType::Area; break;
    case iris::LightType::Point: default: d.type = LightType::Point; break;
    }
    d.colour = Colour(light->color.redF(), light->color.greenF(), light->color.blueF(), 1.0f);
    // A photometric profile multiplies the renderer's attenuation by the raw
    // IES magnitude (peak candela / 1024 * multiplier * ballast factors), which
    // for real luminaires runs into the hundreds. Divide it out here so binding
    // a profile changes the SHAPE of the falloff and not the exposure — the
    // scale factor is recorded on the asset at import time and resolved onto
    // the node beside the path (LightNode::iesNormalisation).
    d.intensity = light->intensity;
    if (!light->iesProfilePath.isEmpty() && light->iesNormalisation > 1e-6f)
        d.intensity = light->intensity / light->iesNormalisation;
    d.range = light->distance;
    d.spotAngleDegrees = light->spotCutOff;
    d.spotSoftness = light->spotCutOffSoftness;
    d.spotFalloff = light->spotFalloff;
    d.rectWidth = light->rectWidth;
    d.rectHeight = light->rectHeight;
    d.doubleSided = light->doubleSided;
    d.accurate = light->accurate;
    // Asset bindings travel as resolved absolute paths — the engine has no
    // database. The backend decides what each light type can actually honour
    // (profiles: spot always, point only unshadowed; masks: approx only).
    d.iesProfilePath = light->iesProfilePath.toStdString();
    d.texturePath    = light->lightTexturePath.toStdString();
    // Area lights never cast shadows (Ogre-Next limitation; the engine enforces
    // it too — this keeps the mirror's shadow-filter bookkeeping honest).
    d.castShadows = light->lightType != iris::LightType::Area &&
                    light->shadowMap && light->shadowMap->shadowType != iris::ShadowMapType::None;
    // Static shadow map (SHADOW_TOOLING_SPEC.md §4.3). Pushed for every light
    // type — the engine decides that it means nothing for directional and area
    // lights, and a mirror that filtered it here would make the document field
    // and the engine's view of it disagree for no gain.
    d.shadowStatic = light->shadowMap && light->shadowMap->staticMap;
    // LIGHTING CHANNELS, light side. The document field is on SceneNode (one
    // field, one meaning, both ends of the test) — the light's copy says which
    // channels it illuminates.
    d.lightMask = light->getLightMask();
    return d;
}

DecalDesc SceneMirror::toDecalDesc(iris::DecalNode *decal)
{
    DecalDesc d;
    d.width = decal->width;
    d.height = decal->height;
    d.depth = decal->depth;
    d.metalness = decal->metalness;
    d.roughness = decal->roughness;
    d.ignoreAlphaDiffuse = decal->ignoreAlphaDiffuse;
    return d;
}

TextureId SceneMirror::decalTextureFor(const QString &path, DecalMap kind)
{
    if (path.isEmpty()) return 0;
    const QString key = QString::number(int(kind)) + '|' + path;
    auto it = mDecalTextures.constFind(key);
    if (it != mDecalTextures.constEnd()) return it.value();
    // Qt resources (":/...") are not files the engine can read.
    if (!QFileInfo::exists(path)) return 0;
    const TextureId id = mTarget->loadDecalTexture(path.toStdString(), kind);
    // Cache failures (0) too — a full atlas or an unreadable file must not be
    // retried on every single frame.
    mDecalTextures.insert(key, id);
    return id;
}

// A decal is pushed like a light: the engine object rides the mirrored node, so
// position, orientation and scale follow the document for free. The image is
// re-bound only when the resolved path set changes (the panel edits guids every
// keystroke; loadDecalTexture resamples a 512x512 atlas slice and is not free).
void SceneMirror::syncDecal(Entry &e, iris::DecalNode *decal)
{
    if (!e.node) return;
    Hasher hs;
    hs << decal->resolvedTexturePath << decal->resolvedNormalPath << decal->resolvedEmissivePath;
    const quint64 sig = hs.h;   // three concatenated QStrings per decal per frame, before
    const bool rebind = !e.hasDecal || e.decalSignature != sig;

    const TextureId diffuse = decalTextureFor(decal->resolvedTexturePath, DecalMap::Diffuse);
    if (!diffuse) {
        // No image (yet), unreadable, or the atlas is full: the node exists and
        // draws its wire box, but projects nothing. Never leave a STALE decal
        // bound — that would keep painting the previous image.
        if (e.hasDecal) { mTarget->removeDecal(e.node); e.hasDecal = false; }
        e.decalSignature = sig;
        return;
    }

    DecalDesc d = toDecalDesc(decal);
    d.diffuse = diffuse;
    d.normal = decalTextureFor(decal->resolvedNormalPath, DecalMap::Normal);
    d.emissive = decalTextureFor(decal->resolvedEmissivePath, DecalMap::Emissive);
    if (mTarget->setDecal(e.node, d)) {
        e.hasDecal = true;
        if (rebind) e.decalSignature = sig;
    }
}

// The wire box: 12 edges of the projector volume plus a tick down -Y showing
// which way it projects. Always on with the helpers toggle (like the area
// light's rectangle, it IS the object's shape, not a falloff volume).
void SceneMirror::syncDecalWires(Entry &e, iris::DecalNode *decal)
{
    if (!mLightWires) {
        if (e.wireNode) mTarget->setNodeVisible(e.wireNode, false);
        return;
    }
    if (!e.wireNode) {
        e.wireNode = mTarget->createNode(e.node);
        // EDITOR HELPER (REFLECTIONS_ADOPTION_SPEC.md P1b): wires, range circles
        // and light icons are things the user must see and a reflection probe
        // must never capture. Marked at CREATION so the very first frame of
        // geometry already carries kHelperBit.
        if (e.wireNode) mTarget->setNodeHelper(e.wireNode, true);
    }
    if (!e.wireNode) return;
    MeshId m = wireMeshFor(4);
    if (!m) return;
    if (!e.wireMaterial) e.wireMaterial = mTarget->createUnlitMaterial(Colour(1, 1, 1), false);
    if (!e.wireMaterial) return;
    if (e.wireKind != 4) { if (mTarget->attachMesh(e.wireNode, m, e.wireMaterial)) e.wireKind = 4; }
    // Amber when the decal projects, dim grey when it has no usable image —
    // the difference between "placed" and "placed but blank" has to be visible.
    pushWireColour(e, e.hasDecal ? Colour(1.0f, 0.75f, 0.2f, 1.0f)
                                 : Colour(0.45f, 0.45f, 0.45f, 1.0f));
    // The wire lives in the decal node's local space: size it to the projector
    // box and undo the node's own scale, exactly as the light wires do (the box
    // mesh is authored as a UNIT cube, so the scale IS the extents).
    const iris::Vec3 s = decal->getLocalScale();
    mTarget->setNodeTransform(e.wireNode, Vec3(), Quat(),
                              Vec3(std::max(decal->width, 0.001f) * (s.x() > 1e-6f ? 1.0f / s.x() : 1.0f),
                                   std::max(decal->depth, 0.001f) * (s.y() > 1e-6f ? 1.0f / s.y() : 1.0f),
                                   std::max(decal->height, 0.001f) * (s.z() > 1e-6f ? 1.0f / s.z() : 1.0f)));
    mTarget->setNodeVisible(e.wireNode, true);
}

bool SceneMirror::toMeshData(iris::Mesh *mesh, MeshData &out)
{
    if (!mesh) return false;
    out = MeshData();
    std::vector<float> tan3, bitan3;
    for (const auto &vb : mesh->getVertexBuffers()) {
        if (!vb || !vb->data) continue;
        const QList<iris::VertexAttribute> attribs = vb->vertexLayout.getAttribs();
        if (attribs.isEmpty()) continue;
        const iris::VertexAttribute &attr = attribs.first();
        const float *f = reinterpret_cast<const float *>(vb->data);
        const int floats = vb->dataSize / int(sizeof(float));
        switch (attr.usage) {
        case iris::VertexAttribUsage::Position:
            out.positions.assign(f, f + floats); break;
        case iris::VertexAttribUsage::Normal:
            out.normals.assign(f, f + floats); break;
        case iris::VertexAttribUsage::Tangent:
            // Authored/assimp-computed tangents (float3, from aiMesh::mTangents).
            // These used to fall into `default:` and be thrown away, so the
            // engine regenerated tangents for every mesh — wrong for models
            // that ship authored TANGENTs (mirrored UVs, baked normal maps).
            tan3.assign(f, f + floats); break;
        case iris::VertexAttribUsage::BiTangent:
            bitan3.assign(f, f + floats); break;
        case iris::VertexAttribUsage::TexCoord0: {
            // assimp stores texcoords as 3 floats; the engine wants 2.
            //
            // V is FLIPPED here: the document keeps assimp's GL-style
            // bottom-left UV origin (the legacy renderer compensated by
            // mirroring every texture image at load — Texture2D::load
            // flipY). The engine samples top-left-origin images unflipped
            // (Ogre convention, same as glTF), so document UVs must arrive
            // as v = 1 - v or every imported model renders its textures
            // V-flipped ("misplaced textures", 2026-08-31). The tangent
            // handedness below negates to match.
            const int comps = attr.count > 0 ? attr.count : 3;
            for (int i = 0; i + comps <= floats; i += comps) { out.uvs.push_back(f[i]); out.uvs.push_back(1.0f - f[i+1]); }
            break;
        }
        default: break;
        }
    }
    if (out.positions.empty()) return false;
    const size_t nv = out.positions.size() / 3;
    if (tan3.size() == nv * 3) {
        // The engine wants float4 tangents (xyz + handedness w). Handedness
        // comes from the bitangent when the document carries one — and is
        // NEGATED relative to the document frame: the V flip above mirrors
        // the bitangent direction (dP/dv changes sign), so the engine-facing
        // w is -sign(dot(cross(n, t), b_document)). Verified against a GLB
        // with authored TANGENT w=+1: assimp's document frame yields -1 here,
        // the negation restores the authored +1 (tests/importer section 3b).
        // Defaults to +1 when the document has no bitangent.
        const bool haveN = out.normals.size() == nv * 3;
        const bool haveB = bitan3.size() == nv * 3;
        out.tangents.resize(nv * 4);
        for (size_t i = 0; i < nv; ++i) {
            const float tx = tan3[i*3], ty = tan3[i*3+1], tz = tan3[i*3+2];
            float w = 1.0f;
            if (haveN && haveB) {
                const float nx = out.normals[i*3], ny = out.normals[i*3+1], nz = out.normals[i*3+2];
                const float cx = ny * tz - nz * ty;
                const float cy = nz * tx - nx * tz;
                const float cz = nx * ty - ny * tx;
                w = (cx * bitan3[i*3] + cy * bitan3[i*3+1] + cz * bitan3[i*3+2] < 0.0f) ? 1.0f : -1.0f;
            }
            out.tangents[i*4] = tx; out.tangents[i*4+1] = ty;
            out.tangents[i*4+2] = tz; out.tangents[i*4+3] = w;
        }
    }
    const iris::IndexBufferPtr ib = mesh->getIndexBuffer();
    if (ib && ib->data && ib->dataSize > 0) {
        const unsigned *idx = reinterpret_cast<const unsigned *>(ib->data);
        out.indices.assign(idx, idx + ib->dataSize / int(sizeof(unsigned)));
    } else {
        out.indices.resize(nv);
        for (size_t i = 0; i < nv; ++i) out.indices[i] = unsigned(i);
    }
    if (out.normals.size() != out.positions.size()) out.normals.clear();
    if (out.uvs.size() != nv * 2) out.uvs.clear();
    return out.indices.size() >= 3;
}

// ---- Rigs: document skeleton -> engine descriptor (GPU_SKINNING_SPEC) --------------

namespace {

/// The tree's ONE TRS decomposition lives in irisgl/core/math/trs.h now (the
/// clip extractor needs the identical function, and two copies of a bone-frame
/// decomposition that must agree bit-for-bit is how a rig comes apart). It
/// still returns the worst |cos| between the normalized basis axes so the
/// caller can warn about shear, which a pos/quat/scale bone cannot represent.
using iris::decomposeTRS;

/// FNV-1a over whatever is fed in — the structure hash behind SkeletonDesc::id.
struct StructureHash {
    unsigned long long h = 1469598103934665603ull;
    void bytes(const void *p, size_t n) {
        const unsigned char *b = static_cast<const unsigned char *>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    }
    void operator()(int v)   { bytes(&v, sizeof(v)); }
    // Quantised so float noise below the tolerance the whole pipeline works to
    // cannot split one rig into two cache entries.
    void operator()(float v) { const int q = int(std::lround(double(v) * 4096.0)); bytes(&q, sizeof(q)); }
    void operator()(const iris::Vec3 &v) { (*this)(v.x()); (*this)(v.y()); (*this)(v.z()); }
    void operator()(const iris::Quat &q) { (*this)(q.x()); (*this)(q.y()); (*this)(q.z()); (*this)(q.scalar()); }
    void operator()(const QString &s) { const QByteArray b = s.toUtf8(); bytes(b.constData(), size_t(b.size())); }
    std::string hex() const { char buf[24]; std::snprintf(buf, sizeof(buf), "%016llx", h); return buf; }
};

}  // namespace

bool SceneMirror::toSkeletonDesc(const iris::SkeletonPtr &skeleton, SkeletonDesc &out)
{
    out = SkeletonDesc();
    if (skeleton.isNull() || skeleton->bones.isEmpty()) return false;

    const QList<iris::BonePtr> &bones = skeleton->bones;
    out.bones.resize(size_t(bones.size()));
    StructureHash hash;
    bool warnedShear = false;

    for (int i = 0; i < bones.size(); ++i) {
        const iris::BonePtr &b = bones[i];
        BoneDesc &bd = out.bones[size_t(i)];
        bd.name = b->name.toStdString();
        // The parent comes from Bone::parentBone, which Mesh::extractSkeleton
        // fills with the NEAREST BONE ANCESTOR in the aiNode tree — skipping the
        // `$AssimpFbx$` pivot nodes that sit between real bones in every
        // pivot-preserving FBX. (Before that fix every bone of such a rig was
        // parentless and this loop would have produced a flat rig.)
        bd.parent = -1;
        if (!b->parentBone.isNull()) {
            const auto it = skeleton->boneMap.constFind(b->parentBone->name);
            if (it != skeleton->boneMap.constEnd() && it.value() != i) bd.parent = it.value();
        }

        // R1: the bind LOCAL that makes the engine's derived reverse bind pose
        // come out equal to assimp's offset matrix. FK over these reproduces
        // meshSpacePoseMatrix, whose inverse IS the offset matrix.
        const iris::Mat4 bindLocal = bd.parent >= 0
            ? bones[bd.parent]->inverseMeshSpacePoseMatrix * b->meshSpacePoseMatrix
            : b->meshSpacePoseMatrix;
        iris::Vec3 p, s; iris::Quat r;
        const float shear = decomposeTRS(bindLocal, p, r, s);
        if (shear > 1e-3f && !warnedShear) {
            warnedShear = true;
            qWarning("toSkeletonDesc: bone '%s' has a sheared bind pose (|cos| %.4f); "
                     "bones are TRS-only and the shear is dropped",
                     qUtf8Printable(b->name), double(shear));
        }
        bd.bindPosition = toVec3(p);
        bd.bindRotation = toQuat(r);
        bd.bindScale    = toVec3(s);

        hash(b->name); hash(bd.parent); hash(p); hash(r); hash(s);
    }
    out.id = hash.hex();
    return true;
}

namespace {

/// Two bind matrices are ONE bone when they agree to within a hair. The
/// tolerance is absolute and generous by rig standards (a millimetre at scene
/// scale): what it must catch is a piece rigged in a DIFFERENT space, not
/// float noise from two exporters writing the same pose.
bool bindAgrees(const iris::Mat4 &a, const iris::Mat4 &b)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::fabs(double(a(r, c)) - double(b(r, c))) > 1e-4) return false;
    return true;
}

}  // namespace

bool SceneMirror::buildUnionSkeleton(const QVector<iris::SkeletonPtr> &pieces,
                                     iris::SkeletonPtr &out, QVector<int> *excludedOut,
                                     QString *why)
{
    out.reset();
    if (excludedOut) excludedOut->clear();
    const auto fail = [&](const QString &msg) { if (why) *why = msg; return false; };
    if (pieces.size() < 2) return fail("fewer than two pieces");

    struct BoneRec {
        iris::Mat4 meshSpace, invMeshSpace;
        QSet<QString> ancestors;     ///< every ancestor ANY piece named, transitively
    };
    QHash<QString, BoneRec> recs;
    int merged = 0;

    // BIGGEST PIECE FIRST, and that is not cosmetic: a bone's bind pose comes
    // from the first piece that carries it, so whichever piece is merged first
    // becomes the REFERENCE every other piece is checked against. Merging in
    // document order would let one 2-bone piece rigged in a foreign space
    // exclude the body and everything that agrees with it. Ordering by bone
    // count (ties by document order, so it stays deterministic) makes the
    // character's largest piece the reference, which is the one a disagreement
    // should be measured against.
    QVector<int> mergeOrder;
    mergeOrder.reserve(pieces.size());
    for (int i = 0; i < pieces.size(); ++i) mergeOrder.append(i);
    std::stable_sort(mergeOrder.begin(), mergeOrder.end(), [&](int a, int b) {
        const int na = pieces[a].isNull() ? 0 : pieces[a]->bones.size();
        const int nb = pieces[b].isNull() ? 0 : pieces[b]->bones.size();
        return na > nb;
    });

    for (int p : mergeOrder) {
        const iris::SkeletonPtr &sk = pieces[p];
        if (sk.isNull() || sk->bones.isEmpty()) {
            if (excludedOut) excludedOut->append(p);
            continue;
        }
        // ONE bind pose per bone or no union for this piece. Checked BEFORE
        // anything is merged, so a disagreeing piece leaves no trace.
        bool agrees = true;
        for (const iris::BonePtr &b : sk->bones) {
            if (b.isNull()) { agrees = false; break; }
            const auto it = recs.constFind(b->name);
            if (it != recs.constEnd() && !bindAgrees(it->meshSpace, b->meshSpacePoseMatrix)) {
                agrees = false;
                break;
            }
        }
        if (!agrees) {
            qWarning("SceneMirror: a rig piece disagrees with its siblings about a shared "
                     "bone's bind pose; it keeps its own rig");
            if (excludedOut) excludedOut->append(p);
            continue;
        }
        for (const iris::BonePtr &b : sk->bones) {
            const bool fresh = !recs.contains(b->name);
            BoneRec &r = recs[b->name];
            if (fresh) {
                r.meshSpace = b->meshSpacePoseMatrix;
                r.invMeshSpace = b->inverseMeshSpacePoseMatrix;
            }
            // The piece's own chain: every bone above this one THAT THIS PIECE
            // carries. Different pieces carry different subsets, so the union of
            // the chains is what the character's real ancestry is.
            for (iris::Bone *up = b->parentBone.data(); up; up = up->parentBone.data()) {
                if (up->name == b->name) break;       // defensive: a self-parent
                r.ancestors.insert(up->name);
            }
        }
        ++merged;
    }
    if (merged < 2) return fail("fewer than two pieces agreed");

    // Ancestry has to be TRANSITIVELY closed before depths mean anything: a
    // piece carrying only {head, root} tells us root is above head, and another
    // carrying {head, neck} tells us neck is; only together do they say
    // root is above neck.
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = recs.begin(); it != recs.end(); ++it) {
            const QSet<QString> direct = it->ancestors;
            for (const QString &a : direct) {
                const auto ar = recs.constFind(a);
                if (ar == recs.constEnd()) continue;
                for (const QString &up : ar->ancestors) {
                    if (up == it.key()) return fail("the pieces describe a cyclic rig");
                    if (!it->ancestors.contains(up)) { it->ancestors.insert(up); changed = true; }
                }
            }
        }
    }

    // The PARENT is the deepest ancestor: with the closure above, "deepest"
    // is simply "the one with the most ancestors of its own", and a tie means
    // two pieces disagree about the hierarchy rather than describing subsets of
    // one — which is a refusal, not a guess.
    QHash<QString, QString> parentOf;
    for (auto it = recs.constBegin(); it != recs.constEnd(); ++it) {
        QString best;
        int bestDepth = -1, ties = 0;
        for (const QString &a : it->ancestors) {
            const auto ar = recs.constFind(a);
            if (ar == recs.constEnd()) continue;
            const int d = ar->ancestors.size();
            if (d > bestDepth) { best = a; bestDepth = d; ties = 1; }
            else if (d == bestDepth) ++ties;
        }
        if (ties > 1) return fail("the pieces disagree about a bone's parent");
        if (!best.isEmpty()) parentOf.insert(it.key(), best);
    }

    // CANONICAL ORDER (depth, then name) — the reason the union hashes the same
    // id whatever order the pieces were visited in.
    QHash<QString, int> depth;
    for (auto it = recs.constBegin(); it != recs.constEnd(); ++it)
        depth.insert(it.key(), it->ancestors.size());
    QVector<QString> order;
    order.reserve(recs.size());
    for (auto it = recs.constBegin(); it != recs.constEnd(); ++it) order.append(it.key());
    std::sort(order.begin(), order.end(), [&](const QString &a, const QString &b) {
        const int da = depth.value(a), db = depth.value(b);
        return da != db ? da < db : a < b;
    });

    auto rig = iris::Skeleton::create();
    for (const QString &name : order) {
        const BoneRec &rec = *recs.constFind(name);
        auto bone = iris::Bone::create(name);
        bone->meshSpacePoseMatrix = rec.meshSpace;
        bone->inverseMeshSpacePoseMatrix = rec.invMeshSpace;
        rig->addBone(bone);
    }
    for (const QString &name : order) {
        const auto pit = parentOf.constFind(name);
        if (pit == parentOf.constEnd()) continue;
        rig->getBone(pit.value())->addChild(rig->getBone(name));
    }
    out = rig;
    return true;
}

bool SceneMirror::rigRemap(const iris::SkeletonPtr &piece, const iris::SkeletonPtr &rig,
                           QVector<unsigned short> &out)
{
    out.clear();
    if (piece.isNull() || rig.isNull()) return false;
    out.reserve(piece->bones.size());
    for (const iris::BonePtr &b : piece->bones) {
        if (b.isNull()) return false;
        const auto it = rig->boneMap.constFind(b->name);
        if (it == rig->boneMap.constEnd()) { out.clear(); return false; }
        out.append((unsigned short)it.value());
    }
    return true;
}

bool SceneMirror::toClipDesc(const iris::ExtractedClip &clip, const std::string &rigId,
                             ClipDesc &out)
{
    out = ClipDesc();
    if (clip.tracks.isEmpty()) return false;
    out.name = clip.name.toStdString();
    out.length = clip.length;

    // The engine's clip-def cache is process-lifetime and keyed by NAME, so the
    // id must be content-derived or a re-imported clip aliases the stale def
    // forever — the same failure class as the VCT datablock-pointer cache.
    // Everything that can change the sampled pose goes into the hash: the rig,
    // the name, the length, and every key of every track.
    StructureHash hash;
    hash(QString::fromStdString(rigId));
    hash(clip.name);
    hash(clip.length);
    out.tracks.reserve(size_t(clip.tracks.size()));
    for (const iris::ClipBoneTrack &track : clip.tracks) {
        BoneTrack bt;
        bt.bone = track.bone;
        bt.keys.reserve(size_t(track.keys.size()));
        hash(track.bone);
        for (const iris::ClipBoneKey &key : track.keys) {
            BoneKey bk;
            bk.time = key.time;
            bk.position = toVec3(key.position);
            bk.rotation = toQuat(key.rotation);
            bk.scale = toVec3(key.scale);
            bt.keys.push_back(bk);
            hash(key.time); hash(key.position); hash(key.rotation); hash(key.scale);
        }
        out.tracks.push_back(std::move(bt));
    }
    out.id = hash.hex();
    return true;
}

// ---- CPU skinning -----------------------------------------------------------------
// Retained as the GPU path's ORACLE only. The bone matrices it takes used to
// come from the document (Skeleton::boneTransforms); the document no longer
// computes a pose, so they come from Scene::boneMatrices — which is what
// HlmsPbs streams into the bone tex buffer, i.e. the shader's own input.

bool SceneMirror::toSkinData(iris::Mesh *mesh, std::vector<float> &boneIndices,
                             std::vector<float> &boneWeights)
{
    boneIndices.clear(); boneWeights.clear();
    if (!mesh) return false;
    for (const auto &vb : mesh->getVertexBuffers()) {
        if (!vb || !vb->data) continue;
        const QList<iris::VertexAttribute> attribs = vb->vertexLayout.getAttribs();
        if (attribs.isEmpty()) continue;
        // Both buffers are 4 floats per vertex (mesh.cpp MAX_BONE_INDICES; indices
        // are stored as floats — the GL shader cast them back with int()).
        const float *f = reinterpret_cast<const float *>(vb->data);
        const int floats = vb->dataSize / int(sizeof(float));
        switch (attribs.first().usage) {
        case iris::VertexAttribUsage::BoneIndices: boneIndices.assign(f, f + floats); break;
        case iris::VertexAttribUsage::BoneWeights: boneWeights.assign(f, f + floats); break;
        default: break;
        }
    }
    return !boneIndices.empty() && boneIndices.size() == boneWeights.size();
}

void SceneMirror::skinVertices(const QVector<iris::Mat4> &boneTransforms,
                               const std::vector<float> &bindPositions,
                               const std::vector<float> &bindNormals,
                               const std::vector<float> &boneIndices,
                               const std::vector<float> &boneWeights,
                               std::vector<float> &outPositions,
                               std::vector<float> &outNormals)
{
    const size_t nv = bindPositions.size() / 3;
    const bool haveNormals = bindNormals.size() == bindPositions.size();
    outPositions = bindPositions;
    outNormals = haveNormals ? bindNormals : std::vector<float>();
    if (boneTransforms.isEmpty() || boneIndices.size() < nv * 4 || boneWeights.size() < nv * 4)
        return;
    // Flatten each bone's skin matrix to row-major 3x4 (iris::Mat4 stores
    // column-major). Row-major keeps the per-vertex loop cache-friendly.
    const int nb = boneTransforms.size();
    std::vector<float> mats(size_t(nb) * 12);
    for (int b = 0; b < nb; ++b) {
        const float *m = boneTransforms[b].constData();   // column-major
        float *d = &mats[size_t(b) * 12];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) d[r * 4 + c] = m[c * 4 + r];
    }
    for (size_t v = 0; v < nv; ++v) {
        const float *bi = &boneIndices[v * 4];
        const float *bw = &boneWeights[v * 4];
        const float wsum = bw[0] + bw[1] + bw[2] + bw[3];
        if (wsum <= 1e-6f) continue;                      // unweighted: stay at bind pose
        // Weighted sum of bone matrices, then transform — exactly the GL shader
        // (pbr_material.vert): boneMatrix = sum(u_bones[idx] * weight).
        float B[12] = { 0 };
        for (int k = 0; k < 4; ++k) {
            const int idx = int(bi[k]);
            if (bw[k] == 0.0f || idx < 0 || idx >= nb) continue;
            const float w = bw[k];
            const float *m = &mats[size_t(idx) * 12];
            for (int j = 0; j < 12; ++j) B[j] += m[j] * w;
        }
        const float px = bindPositions[v*3], py = bindPositions[v*3+1], pz = bindPositions[v*3+2];
        outPositions[v*3]   = B[0]*px + B[1]*py + B[2]*pz  + B[3];
        outPositions[v*3+1] = B[4]*px + B[5]*py + B[6]*pz  + B[7];
        outPositions[v*3+2] = B[8]*px + B[9]*py + B[10]*pz + B[11];
        if (haveNormals) {
            const float nx = bindNormals[v*3], ny = bindNormals[v*3+1], nz = bindNormals[v*3+2];
            float ox = B[0]*nx + B[1]*ny + B[2]*nz;
            float oy = B[4]*nx + B[5]*ny + B[6]*nz;
            float oz = B[8]*nx + B[9]*ny + B[10]*nz;
            const float len = std::sqrt(ox*ox + oy*oy + oz*oz);
            if (len > 1e-8f) { ox /= len; oy /= len; oz /= len; }
            outNormals[v*3] = ox; outNormals[v*3+1] = oy; outNormals[v*3+2] = oz;
        }
    }
}

namespace {

/// The nearest ancestor-or-self carrying a SKELETAL clip. That node is both the
/// clip's owner and the root of the subtree its channels address — the document
/// evaluator starts its walk there, so clip translation must too.
iris::SceneNode *clipHostOf(iris::SceneNode *node)
{
    for (iris::SceneNode *n = node; n; n = n->getParent().data()) {
        for (const auto &anim : n->getAnimations())
            if (!anim.isNull() && anim->hasSkeletalAnimation()) return n;
    }
    return nullptr;
}

/// The nearest ancestor-or-self carrying a LOCOMOTION component, and the state
/// machine on it (AVATAR_LOCOMOTION_SPEC Stage 5). Walks UP for the same reason
/// clipHostOf does: `avatar.spawn` puts the movement and locomotion components
/// on the avatar WRAPPER, and the rigged mesh that owns the engine rig is a
/// descendant of it. Null for every node that is not part of an avatar, which is
/// how the authored transport keeps the whole rest of the scene.
const iris::AvatarLocomotion *locomotionHostOf(iris::SceneNode *node)
{
    for (iris::SceneNode *n = node; n; n = n->getParent().data())
        if (n->hasLocomotionComponent()) return n->locomotion();
    return nullptr;
}

}  // namespace

const iris::SceneNode *SceneMirror::characterHostOf(iris::SceneNode *node) const
{
    if (!node) return nullptr;
    // The clip host first: it is the node the character's clips live on and the
    // root of the subtree their channels address, so it is exactly "the
    // character" for every imported model that carries an animation.
    if (iris::SceneNode *host = clipHostOf(node)) return host;
    // No clips anywhere above: the pieces' common PARENT is the character. Not
    // the scene root, though — two unrelated single-piece characters dropped
    // side by side are not one character, and unioning their rigs would merge
    // two strangers' skeletons into one 130-bone rig that neither of them is.
    const iris::SceneNodePtr parent = node->getParent();
    if (parent.isNull()) return nullptr;
    if (!mSource.isNull() && parent.data() == mSource->getRootNode().data()) return nullptr;
    return parent.data();
}

const SceneMirror::CharacterRig *SceneMirror::characterRigFor(iris::SceneNode *piece)
{
    const iris::SceneNode *host = characterHostOf(piece);
    if (!host) return nullptr;

    // The character's skinned pieces, in document order — a walk of the host's
    // subtree, which sees every piece whether or not it has been mirrored yet.
    // That matters: the union has to be COMPLETE the first time any piece
    // attaches, or the pieces that attached early would all need a re-attach.
    QVector<iris::SkeletonPtr> skels;
    quint64 sig = 1469598103934665603ull;
    const auto hashPtr = [&sig](const void *p) {
        quint64 v = quint64(quintptr(p));
        for (int i = 0; i < 8; ++i) { sig ^= (v & 0xFF); sig *= 1099511628211ull; v >>= 8; }
    };
    QVector<iris::SceneNode *> stack;
    stack.append(const_cast<iris::SceneNode *>(host));
    for (int i = 0; i < stack.size(); ++i) {
        iris::SceneNode *n = stack[i];
        if (n->getSceneNodeType() == iris::SceneNodeType::Mesh) {
            auto *mn = static_cast<iris::MeshNode *>(n);
            if (!mn->skeleton.isNull()) { skels.append(mn->skeleton); hashPtr(mn->skeleton.data()); }
        }
        const int cn = n->childCount();
        for (int k = 0; k < cn; ++k) if (iris::SceneNode *c = n->childAt(k)) stack.append(c);
    }
    if (skels.size() < 2) {                       // a lone piece keeps its own rig
        mCharacterRigs.remove(host);
        return nullptr;
    }

    CharacterRig &rec = mCharacterRigs[host];
    if (rec.signature == sig && !rec.rig.isNull()) return &rec;

    iris::SkeletonPtr rig;
    QVector<int> excluded;
    QString why;
    if (!buildUnionSkeleton(skels, rig, &excluded, &why)) {
        qWarning("SceneMirror: '%s' keeps per-piece rigs (%s)",
                 qUtf8Printable(const_cast<iris::SceneNode *>(host)->getName()),
                 qUtf8Printable(why));
        const quint32 epoch = rec.epoch + 1;      // pieces on the old union must re-attach
        rec = CharacterRig();
        rec.host = host;
        rec.signature = sig;
        rec.epoch = epoch;
        return nullptr;
    }
    SkeletonDesc desc;
    if (!toSkeletonDesc(rig, desc)) { mCharacterRigs.remove(host); return nullptr; }

    CharacterRig fresh;
    fresh.host = host;
    fresh.signature = sig;
    fresh.rig = rig;
    fresh.rigId = desc.id;
    // The epoch only moves when the derived rig really CHANGED: a piece whose
    // entry was released and re-adopted must not drag the whole character
    // through a re-attach because a pointer moved.
    fresh.epoch = rec.epoch + (rec.rigId == fresh.rigId ? 0 : 1);
    for (int i = 0; i < skels.size(); ++i) {
        if (excluded.contains(i)) continue;
        QVector<unsigned short> map;
        if (rigRemap(skels[i], rig, map)) fresh.remaps.insert(skels[i].data(), map);
    }
    rec = fresh;
    return &rec;
}

void SceneMirror::attachClipsFor(Entry &e)
{
    // THE RIG SKELETON, not the piece's own: bone TRACK indices are resolved
    // against the rig the engine holds, which for a piece of a multi-piece
    // character is the character union (AVATAR_RIG_PERF_SPEC §3.1).
    if (!e.gpuSkinned || !e.docNode || e.rigSkeleton.isNull()) return;
    if (e.shareMaster) return;                  // a follower carries no clips
    iris::SceneNode *host = clipHostOf(e.docNode);

    // The signature covers everything that can change what the engine should be
    // holding: the rig, and the identity + length of every clip. It does NOT
    // cover the clip's keys — those are inside the per-clip content id, which is
    // what the engine's process-lifetime def cache is keyed on.
    QString signature = QString::fromStdString(e.rigId);
    QList<iris::AnimationPtr> clips;
    if (host) {
        QList<iris::AnimationPtr> candidates = host->getAnimations();
        // The ACTIVE animation is not always IN that list. The Avatar page
        // rebuilds each clip through buildClipAnimation (root motion is a
        // preview policy, so the played clip is a stripped copy of the authored
        // one) and hands the copy to setAnimation without adding it. Attaching
        // only the listed ones would leave the played clip with no engine
        // translation at all — the character would sit at bind pose while the
        // transport ran.
        const iris::AnimationPtr active = host->getAnimation();
        if (!active.isNull() && !candidates.contains(active)) candidates.append(active);
        for (const auto &anim : candidates) {
            if (anim.isNull() || !anim->hasSkeletalAnimation()) continue;
            clips.append(anim);
            // The POINTER is part of the signature on purpose: the Avatar
            // page's root-motion toggle rebuilds every clip's Animation object
            // with the same name and length, and without this the mirror would
            // keep playing the pre-toggle translation.
            signature += QLatin1Char('|') + anim->getName() +
                         QLatin1Char(':') + QString::number(double(anim->getLength()), 'g', 6) +
                         QLatin1Char('@') + QString::number(quintptr(anim.data()), 16);
        }
    }
    if (signature == e.clipSignature) return;
    e.clipSignature = signature;
    e.lastClipPush.clear();
    if (clips.isEmpty() || !host) {
        e.clipMap.clear();
        e.clipIdMap.clear();
        e.clipNameMap.clear();
        return;
    }

    // R2 again, from the host side: clips ACCUMULATE on a node — the Avatar
    // page loads a Mixamo animation onto an already-loaded character — so this
    // runs while the previous set is attached AND, quite possibly, playing.
    // Ogre's addAnimationsFromSkeleton would go stale every active-animation
    // pointer, so the engine refuses; disable everything first. (Found by
    // scripting.e2e.avatar the moment a cross-file clip was added: the
    // character froze at bind pose with one warning in the log.)
    ++mClipStatePushes;
    mTarget->setClipStates(e.node, nullptr, 0);

    // The pivot composition, once per (rig, clip): §3.1's "compose then
    // resample". Its cost is O(bones x keys) and it happens here rather than per
    // frame precisely because it is not cheap.
    std::vector<ClipDesc> descs;
    QVector<QPair<const iris::Animation *, QString>> pushed;   // (animation, clip id), in push order
    descs.reserve(size_t(clips.size()));
    QVector<iris::ExtractedClip> extracted(clips.size());
    const std::vector<std::string> before = mTarget->clipNames(e.node);
    for (int i = 0; i < clips.size(); ++i) {
        QString err;
        if (!iris::ClipExtractor::extract(host->sharedFromThis(), e.docNode->sharedFromThis(),
                                          e.rigSkeleton, clips[i]->getSkeletalAnimation(),
                                          clips[i]->getName(), clips[i]->getLength(),
                                          nullptr, extracted[i], &err)) {
            qWarning("SceneMirror: clip '%s' did not translate: %s",
                     qUtf8Printable(clips[i]->getName()), qUtf8Printable(err));
            continue;
        }
        if (!extracted[i].restDiffersFromBind.isEmpty()) {
            // Not fatal, and worth saying out loud: the document composes an
            // untouched bone from its authored REST transform, while an engine
            // skeleton resets one to its BIND pose. They coincide in every file
            // we have; where they do not, a bone no clip mentions lands
            // somewhere other than the document put it.
            qWarning("SceneMirror: clip '%s' — %lld bone(s) whose authored rest differs from "
                     "their bind pose (first: %s)", qUtf8Printable(clips[i]->getName()),
                     (long long)extracted[i].restDiffersFromBind.size(),
                     qUtf8Printable(extracted[i].restDiffersFromBind.first()));
        }
        ClipDesc desc;
        if (!toClipDesc(extracted[i], e.rigId, desc)) continue;
        pushed.append({ clips[i].data(), QString::fromStdString(desc.id) });
        descs.push_back(std::move(desc));
    }
    if (descs.empty()) {
        e.clipMap.clear();
        e.clipIdMap.clear();
        e.clipNameMap.clear();
        return;
    }

    // R2: every clip goes on BEFORE any is enabled. Ogre's
    // addAnimationsFromSkeleton reallocates the vector its active-animation list
    // holds raw pointers into and does not fix that list up, so attaching to a
    // playing node goes stale every one of them. Nothing is enabled yet at this
    // point (the state push below is what enables), so one batched call is all
    // that is needed — and the engine refuses the alternative anyway.
    if (!mTarget->attachClips(e.node, descs.data(), descs.size())) {
        qWarning("SceneMirror: attachClips failed for a skinned node; it will render at bind pose");
        e.clipMap.clear();
        e.clipIdMap.clear();
        e.clipNameMap.clear();
        return;
    }
    // Whatever the engine appended, in the order it was pushed, is the mapping
    // for the clips that were NOT already there. Names it already knew keep
    // their existing mapping.
    QStringList added;
    {
        QSet<QString> had;
        for (const auto &n : before) had.insert(QString::fromStdString(n));
        for (const auto &n : mTarget->clipNames(e.node)) {
            const QString name = QString::fromStdString(n);
            if (!had.contains(name)) added.append(name);
        }
    }
    // Names appear in the engine's list once per DISTINCT content id, in push
    // order — so walk the pushed ids, skipping ones already mapped and ones
    // repeated within this batch, and pair each remaining first occurrence with
    // the next new engine name.
    int next = 0;
    for (const auto &entry : pushed) {
        e.clipMap.insert(entry.first, entry.second);
        if (e.clipIdMap.contains(entry.second)) continue;
        if (next < added.size()) e.clipIdMap.insert(entry.second, added[next++]);
    }
    // ...and the NAME hop the locomotion push needs (Stage 5). Same pass, same
    // order, so the two lookups can never disagree about which engine clip a
    // document clip is; FIRST WINS on a duplicate name, which is the rule
    // `collectAvatarClips` applies at the other end.
    e.clipNameMap.clear();
    for (const auto &entry : pushed) {
        const QString docName = entry.first->getName();
        if (docName.isEmpty() || e.clipNameMap.contains(docName)) continue;
        const auto engineName = e.clipIdMap.constFind(entry.second);
        if (engineName != e.clipIdMap.constEnd())
            e.clipNameMap.insert(docName, engineName.value());
    }
}

bool SceneMirror::entryBoneWorldTransforms(const Entry &e, QHash<QString, iris::Mat4> &out) const
{
    if (!mTarget) return false;
    if (!e.gpuSkinned || e.rigSkeleton.isNull() || !e.docNode) return false;
    const QList<iris::BonePtr> &bones = e.rigSkeleton->bones;
    if (bones.isEmpty() || size_t(bones.size()) != e.boneCount) return false;
    // The three scratch buffers are MEMBERS, not locals: this used to run once
    // per bone-overlay refresh, and now it runs every frame for every rig that
    // carries a socket. assign()/resize() on a member keeps the capacity, so
    // the steady state is zero allocations here.
    mPoseScratch.assign(e.boneCount, BonePose());
    if (!mTarget->bonePoses(e.node, mPoseScratch.data(), mPoseScratch.size())) return false;
    std::vector<BonePose> &poses = mPoseScratch;

    // The engine hands back parent-local TRS; the FK back up to world is
    // ours. Bone order is free (a parent may follow its child), so the
    // derived matrices are resolved by walking each bone's own ancestry
    // rather than assuming the array is topologically sorted.
    const iris::Mat4 meshWorld = e.docNode->getGlobalTransform();
    const size_t n = size_t(bones.size());
    mDerivedScratch.assign(n, iris::Mat4());
    mDerivedDone.assign(n, 0);
    // PARENT INDICES, RESOLVED ONCE PER RIG (audit F13). This ran per rigged
    // node per frame the moment the scene had a socket, and the parent lookup
    // was a QHash<QString> probe keyed on the parent bone's NAME — a string
    // hash per bone per frame for a relationship that is fixed by the rig. The
    // cache is keyed on the skeleton pointer and the bone count, so a
    // re-imported rig rebuilds it.
    if (e.boneParentsOwner != e.rigSkeleton.data() || e.boneParents.size() != n) {
        e.boneParents.assign(n, -1);
        for (size_t i = 0; i < n; ++i) {
            if (bones[int(i)]->parentBone.isNull()) continue;
            const auto pit = e.rigSkeleton->boneMap.constFind(bones[int(i)]->parentBone->name);
            if (pit != e.rigSkeleton->boneMap.constEnd() && size_t(pit.value()) != i)
                e.boneParents[i] = pit.value();
        }
        e.boneParentsOwner = e.rigSkeleton.data();
    }
    // ...and the FK itself is a member function, not a recursive
    // std::function: the closure captured six references, which is past
    // libstdc++'s small-object buffer, so building it was a HEAP ALLOCATION per
    // rigged node per frame, and every bone paid an indirect call on top.
    for (size_t i = 0; i < n; ++i) resolveBoneDerived(int(i), e.boneParents, poses);
    for (int i = 0; i < bones.size(); ++i)
        out.insert(bones[i]->name, meshWorld * mDerivedScratch[size_t(i)]);
    return true;
}

const iris::Mat4 &SceneMirror::resolveBoneDerived(int i, const std::vector<int> &parents,
                                                  const std::vector<BonePose> &poses) const
{
    if (mDerivedDone[size_t(i)]) return mDerivedScratch[size_t(i)];
    mDerivedDone[size_t(i)] = 1;           // cycles are impossible by rig contract; guard anyway
    const BonePose &p = poses[size_t(i)];
    iris::Mat4 local;
    local.translate(iris::Vec3(p.position.x, p.position.y, p.position.z));
    local.rotate(iris::Quat(p.rotation.w, p.rotation.x, p.rotation.y, p.rotation.z));
    local.scale(iris::Vec3(p.scale.x, p.scale.y, p.scale.z));
    const int parent = parents[size_t(i)];
    // The parent's matrix is read into a LOCAL before the write: the recursion
    // can resize nothing (the scratch is sized above) but it can write
    // mDerivedScratch[parent], and taking a reference across that write is the
    // kind of aliasing that only shows up under a different inliner.
    if (parent >= 0) {
        const iris::Mat4 parentWorld = resolveBoneDerived(parent, parents, poses);
        mDerivedScratch[size_t(i)] = parentWorld * local;
    } else {
        mDerivedScratch[size_t(i)] = local;
    }
    return mDerivedScratch[size_t(i)];
}

bool SceneMirror::boneWorldTransforms(QHash<QString, iris::Mat4> &out) const
{
    out.clear();
    if (!mTarget) return false;
    bool any = false;
    for (auto it = mEntries.constBegin(); it != mEntries.constEnd(); ++it)
        if (entryBoneWorldTransforms(*it, out)) any = true;
    return any;
}

bool SceneMirror::boneWorldTransforms(iris::SceneNode *node, QHash<QString, iris::Mat4> &out) const
{
    out.clear();
    if (!mTarget || !node) return false;
    const auto it = mEntries.constFind(node);
    if (it == mEntries.constEnd()) return false;
    return entryBoneWorldTransforms(it.value(), out);
}

int SceneMirror::resolveSockets()
{
    // THE RECONCILER (AVATAR_RIG_PERF_SPEC §4.2). This used to be a per-frame
    // RESOLVER: read every rigged owner's pose back from the engine, run FK over
    // it on the CPU, and write each rider's world transform — one frame late by
    // construction, and paid every frame by every character carrying a socket.
    //
    // Now the ENGINE does it: a rider hangs off a TagPoint on the owner's bone,
    // which Ogre resolves inside its threaded update, in the frame that renders
    // (updateAllTagPoints). So the per-frame socket cost on our side is ZERO and
    // this function only has to keep the engine's arrangement equal to the
    // document's — arm a tag when an attachment appears, move it when the socket
    // is edited, free it when anything goes away.
    //
    // The bind-pose fallback stays exactly as it was for an owner with no live
    // engine rig (a headless run, a character the walk has not rigged yet):
    // those riders are still moved by writing their world transform.
    if (!mSource) return 0;
    if (!mTarget) return mSockets.resolve(mSource.data());
    return reconcileSockets();
}

namespace {

/// A cheap key for a socket's offset — the reconciler pushes a new offset only
/// when the numbers really moved.
quint64 offsetKeyOf(const iris::Socket &socket)
{
    quint64 h = 1469598103934665603ull;
    const auto mix = [&h](float f) {
        quint32 bits;
        std::memcpy(&bits, &f, sizeof(bits));
        for (int i = 0; i < 4; ++i) { h ^= (bits & 0xFF); h *= 1099511628211ull; bits >>= 8; }
    };
    mix(socket.position.x()); mix(socket.position.y()); mix(socket.position.z());
    mix(socket.rotation.x()); mix(socket.rotation.y()); mix(socket.rotation.z());
    mix(socket.rotation.scalar());
    mix(socket.scale.x()); mix(socket.scale.y()); mix(socket.scale.z());
    return h;
}

}  // namespace

int SceneMirror::reconcileSockets()
{
    mSocketDangling = 0;
    int riding = 0;
    const QHash<QString, QList<iris::SceneNodePtr>> &attachments = mSource->socketAttachments;
    mRidersSeen.clear();
    QSet<const iris::SceneNode *> &seen = mRidersSeen;

    for (auto it = attachments.constBegin(); it != attachments.constEnd(); ++it) {
        const QList<iris::SceneNodePtr> &attached = it.value();
        if (attached.isEmpty()) continue;
        const iris::SceneNodePtr ownerNode = mSource->nodes.value(it.key());
        iris::MeshNode *owner = (!ownerNode.isNull() &&
                                 ownerNode->getSceneNodeType() == iris::SceneNodeType::Mesh)
                                    ? static_cast<iris::MeshNode *>(ownerNode.data())
                                    : nullptr;
        // Is there a LIVE ENGINE RIG to hang a tag on? Without one (a headless
        // run, a character the walk has not rigged yet) the riders take the
        // document path below, at the bind pose — unchanged behaviour.
        const auto ownerEntry = owner ? mEntries.constFind(owner) : mEntries.constEnd();
        const bool rigged = ownerEntry != mEntries.constEnd() && ownerEntry->gpuSkinned &&
                            ownerEntry->node;

        for (const iris::SceneNodePtr &riderPtr : attached) {
            iris::SceneNode *rider = riderPtr.data();
            if (!rider) { ++mSocketDangling; continue; }
            const iris::Socket *socket = owner ? owner->findSocket(rider->socketName) : nullptr;
            if (!socket) { ++mSocketDangling; releaseRider(rider); continue; }
            seen.insert(rider);

            if (!rigged) {
                releaseRider(rider);        // it may have been on a tag a moment ago
                iris::Mat4 world;
                if (!iris::socketWorldTransform(owner, rider->socketName,
                                                iris::BonePoseSource(), world)) {
                    ++mSocketDangling;
                    continue;
                }
                // The write below lands in the rider's LOCAL, which since D4 is
                // its offset from the socket — so the authored value is kept
                // first and restored the moment a tag can be armed.
                RiderState &st = mBoneRiders[rider];
                if (!st.fallbackDriven) {
                    st.fallbackDriven = true;
                    st.authoredPos = rider->getLocalPos();
                    st.authoredRot = rider->getLocalRot();
                    st.authoredScale = rider->getLocalScale();
                }
                rider->setGlobalTransform(world);
                rider->update(0.0f);
                ++riding;
                continue;
            }

            const auto riderEntry = mEntries.constFind(rider);
            if (riderEntry == mEntries.constEnd() || !riderEntry->node) {
                // Not mirrored yet — the walk that follows this call creates it
                // and the next sync arms it. Not a dangle.
                continue;
            }
            const jahshaka::engine::NodeId riderId = riderEntry->node;
            const jahshaka::engine::NodeId ownerId = ownerEntry->node;
            const quint64 key = offsetKeyOf(*socket);

            // CHANGE-GUARDED AGAINST THE ENGINE, not against a mirror latch: the
            // engine frees a tag by itself whenever the owner's skeleton is
            // replaced (an Item re-created, a share armed or dropped), and a
            // latch would then believe in an attachment that no longer exists.
            std::string actualBone;
            const jahshaka::engine::NodeId actualOwner =
                mTarget->boneAttachment(riderId, &actualBone);
            const bool armed = actualOwner == ownerId &&
                               actualBone == socket->boneName.toStdString();
            RiderState &state = mBoneRiders[rider];
            if (!armed) {
                // The rider keeps its place in the DOCUMENT hierarchy — the
                // socket API's promise — through the graph layer's shadow
                // parent, registered BEFORE the tag so no reader ever sees a
                // TagPoint as a parent.
                const iris::SceneNodePtr parent = rider->getParent();
                if (!parent.isNull())
                    iris::graph::setSocketRider(rider->graphNode(), parent->graphNode());
                if (!mTarget->attachToBone(riderId, ownerId, socket->boneName.toStdString(),
                                           toVec3(socket->position), toQuat(socket->rotation),
                                           toVec3(socket->scale))) {
                    iris::graph::clearSocketRider(rider->graphNode());
                    mBoneRiders.remove(rider);
                    ++mSocketDangling;
                    continue;
                }
                state.owner = ownerId;
                state.bone = socket->boneName;
                state.offsetKey = key;
                // Out of the fallback: the world transform the fallback wrote
                // into this node's local is not a socket offset, and the tag
                // would ride it. The authored local goes back.
                if (state.fallbackDriven) {
                    state.fallbackDriven = false;
                    rider->setLocalPos(state.authoredPos);
                    rider->setLocalRot(state.authoredRot);
                    rider->setLocalScale(state.authoredScale);
                }
                state.lastLocalPos = rider->getLocalPos();
                state.lastLocalRot = rider->getLocalRot();
                state.lastLocalScale = rider->getLocalScale();
            } else if (state.offsetKey != key) {
                mTarget->setBoneAttachmentOffset(riderId, toVec3(socket->position),
                                                 toQuat(socket->rotation), toVec3(socket->scale));
                state.offsetKey = key;
                state.owner = ownerId;
                state.bone = socket->boneName;
            }
            // What the rider's local is, as of this sync — the reference the
            // release compares against (see RiderState::lastLocal*).
            state.lastLocalPos = rider->getLocalPos();
            state.lastLocalRot = rider->getLocalRot();
            state.lastLocalScale = rider->getLocalScale();
            ++riding;
        }
    }

    // The stale half is NOT done here — see sweepStaleRiders, which runs at the
    // END of sync for a reason that cost a SEGV to find.
    return riding;
}

void SceneMirror::releaseAllRiders()
{
    if (!mTarget || mBoneRiders.isEmpty()) return;
    const QList<const iris::SceneNode *> riders = mBoneRiders.keys();
    for (const iris::SceneNode *rider : riders)
        releaseRider(const_cast<iris::SceneNode *>(rider));   // reads the row, then removes it
    mBoneRiders.clear();
    mRidersSeen.clear();
}

void SceneMirror::sweepStaleRiders()
{
    // ANYTHING WE ARMED THAT THE DOCUMENT NO LONGER ATTACHES comes off its bone
    // and back under its document parent, at the pose it last rendered with —
    // "it keeps its last pose", which is what the socket API promises for a
    // stale attachment.
    //
    // AND IT RUNS AT THE END OF sync(), AFTER removeMissing. `mBoneRiders` is
    // keyed by document node POINTER (the reconciler needs the node, not an id),
    // and a rider that was DELETED from the document is a freed pointer in that
    // map until removeMissing drops it. Sweeping at the top of sync — where the
    // reconciler itself runs — therefore dereferenced a destroyed node the frame
    // after a socketed prop was deleted, which is exactly the crash the
    // "rider deleted mid-ride" case in sockets.tags reproduces.
    if (!mTarget || mBoneRiders.isEmpty()) return;
    // COLLECT FIRST, RELEASE AFTER: releaseRider READS this rider's record (the
    // local TRS the mirror last saw, which is how "the caller placed it since"
    // is decided) and removes it itself. Erasing the row here first threw that
    // record away and made every release look like "nothing was written", which
    // silently overwrote a transform the caller had just set.
    QVector<iris::SceneNode *> stale;
    for (auto it = mBoneRiders.constBegin(); it != mBoneRiders.constEnd(); ++it)
        if (!mRidersSeen.contains(it.key())) stale.append(const_cast<iris::SceneNode *>(it.key()));
    for (iris::SceneNode *rider : stale) releaseRider(rider);
}

void SceneMirror::releaseRider(iris::SceneNode *rider)
{
    if (!rider || !mTarget) return;
    const iris::SceneNodePtr parent = rider->getParent();

    // DID THE CALLER PLACE IT since the last sync? "Detaching is also how you
    // put something where a bone was" — and then move it — so an explicit write
    // wins over the pose-keeping bake below. Captured BEFORE anything touches
    // the node: the engine's own detach re-expresses the local so that the world
    // is preserved, which would overwrite exactly the write being looked for.
    const iris::Vec3 callerPos = rider->getLocalPos(), callerScale = rider->getLocalScale();
    const iris::Quat callerRot = rider->getLocalRot();
    bool placedByCaller = false;
    {
        const auto st = mBoneRiders.constFind(rider);
        if (st != mBoneRiders.constEnd()) {
            const auto differs = [](float a, float b) { return std::fabs(double(a) - double(b)) > 1e-5; };
            placedByCaller =
                differs(callerPos.x(), st->lastLocalPos.x()) ||
                differs(callerPos.y(), st->lastLocalPos.y()) ||
                differs(callerPos.z(), st->lastLocalPos.z()) ||
                differs(callerScale.x(), st->lastLocalScale.x()) ||
                differs(callerScale.y(), st->lastLocalScale.y()) ||
                differs(callerScale.z(), st->lastLocalScale.z()) ||
                differs(callerRot.x(), st->lastLocalRot.x()) ||
                differs(callerRot.y(), st->lastLocalRot.y()) ||
                differs(callerRot.z(), st->lastLocalRot.z()) ||
                differs(callerRot.scalar(), st->lastLocalRot.scalar());
        }
    }

    const auto entry = mEntries.constFind(rider);
    if (entry != mEntries.constEnd() && entry->node) {
        // Off the bone: the engine re-homes the node under whatever parent it
        // can name and re-expresses the pose it last rendered with.
        jahshaka::engine::NodeId parentId = 0;
        if (!parent.isNull()) {
            const auto pe = mEntries.constFind(parent.data());
            if (pe != mEntries.constEnd()) parentId = pe->node;
        }
        mTarget->detachFromBone(entry->node, parentId);
    }
    iris::graph::clearSocketRider(rider->graphNode());
    mBoneRiders.remove(rider);

    // ...AND BACK INTO THE DOCUMENT TREE, in Ogre's hierarchy and not only in
    // the shadow bookkeeping. The engine can only re-home the node under a node
    // it has an id for, and the document's ROOT is not mirrored (the walk starts
    // at its children) — so a rider whose parent is the root would be left as a
    // sibling of the document tree instead of inside it. That is invisible until
    // the document MIGRATES (a mirror unbinding, an editor/player swap), which
    // walks the tree from its root: the stray node stays behind in a scene
    // manager the caller is about to destroy, and the document node holding its
    // handle faults at its own destruction. sockets.render's teardown found it.
    const iris::Mat4 world = rider->getGlobalTransform();
    if (!parent.isNull() && rider->graphNode() && parent->graphNode() &&
        iris::graph::parentOf(rider->graphNode()) != parent->graphNode())
        iris::graph::attach(parent->graphNode(), rider->graphNode(), -1);
    if (placedByCaller) {
        rider->setLocalPos(callerPos);
        rider->setLocalRot(callerRot);
        rider->setLocalScale(callerScale);
    } else {
        rider->setGlobalTransform(world);   // "it keeps the pose it was last resolved to"
    }
}

namespace {

/// Two nodes are in the SAME PLACE when their world transforms agree. The
/// tolerance is loose enough for the float noise two identical TRS compositions
/// can produce and far tighter than any authored offset.
bool sameWorld(const iris::Mat4 &a, const iris::Mat4 &b)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::fabs(double(a(r, c)) - double(b(r, c))) > 1e-4) return false;
    return true;
}

}  // namespace

void SceneMirror::syncSkeletonSharing()
{
    // THE SECOND HALF of the character rig (AVATAR_RIG_PERF_SPEC §3.4): the
    // pieces of one character render from ONE SkeletonInstance, so the pose is
    // evaluated once and the clips are pushed once, instead of once per piece.
    //
    // WHO MAY SHARE. Ogre's shared bones carry the MASTER's node transform
    // (OgreItem.h:200-205), so a piece may only share while it sits exactly
    // where the master does — which is what an imported character's pieces do
    // (§0.4: every piece at identity under the model root). A user who MOVES a
    // piece un-shares it on the next sync and it goes back to its own instance,
    // correct and slower; moving it back re-shares it.
    //
    // Everything here is CHANGE-GUARDED against the engine's own answer
    // (sharesSkeleton), not against a mirror-side latch, because the engine
    // drops a share by itself whenever an Item is re-created — a material swap,
    // a mesh swap — and a latch would then believe in a share that no longer
    // exists.
    if (!mTarget || mEntries.isEmpty()) return;

    mShareGroups.clear();
    for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
        Entry &e = *it;
        // `characterHost` is set only for a piece that took a UNION rig, which
        // is exactly the multi-piece case: a single-piece character has nothing
        // to share and pays nothing here.
        if (!e.gpuSkinned || !e.node || !e.characterHost || !e.docNode) continue;
        mShareGroups[e.characterHost].append(&e);
    }

    for (auto g = mShareGroups.begin(); g != mShareGroups.end(); ++g) {
        QVector<Entry *> &group = g.value();
        if (group.size() < 2) continue;
        // THE MASTER: the piece with the most bones of its own (the body, on a
        // real character), tie-broken by NAME so the choice is deterministic
        // across runs and machines — the alternative is a pointer order, and a
        // master that changes between two identical loads would change which
        // node the whole character renders from.
        Entry *master = group[0];
        for (Entry *e : group) {
            if (e->blendToRig.size() > master->blendToRig.size()) { master = e; continue; }
            if (e->blendToRig.size() == master->blendToRig.size() &&
                e->docNode->getName() < master->docNode->getName())
                master = e;
        }
        // A master that is itself sharing (its geometry changed and the group
        // re-formed around it) has to be freed first — sharing is not chained.
        if (mTarget->sharesSkeleton(master->node)) {
            mTarget->shareSkeleton(master->node, 0);
            master->shareMaster = 0;
            master->clipSignature.clear();      // it owns its clips again
            master->lastClipPush.clear();
        }
        const iris::Mat4 masterWorld = master->docNode->getGlobalTransform();

        for (Entry *e : group) {
            if (e == master) continue;
            const bool eligible = e->rigId == master->rigId &&
                                  sameWorld(e->docNode->getGlobalTransform(), masterWorld);
            const bool shared = mTarget->sharesSkeleton(e->node);
            if (eligible && (!shared || e->shareMaster != master->node)) {
                if (mTarget->shareSkeleton(e->node, master->node)) {
                    e->shareMaster = master->node;
                    // A follower holds NO clips (the engine drops them when the
                    // instance goes): forget what we think it has, so that if it
                    // ever un-shares the clip pass re-attaches from scratch.
                    e->clipSignature.clear();
                    e->clipMap.clear();
                    e->clipIdMap.clear();
                    e->clipNameMap.clear();
                    e->lastClipPush.clear();
                } else {
                    e->shareMaster = 0;
                }
            } else if (!eligible && shared) {
                mTarget->shareSkeleton(e->node, 0);
                e->shareMaster = 0;
                e->clipSignature.clear();       // it needs its own clips again
                e->clipMap.clear();
                e->clipIdMap.clear();
                e->clipNameMap.clear();
                e->lastClipPush.clear();
            } else {
                e->shareMaster = shared ? master->node : 0;
            }
        }
    }
}

void SceneMirror::syncClips()
{
    // The per-frame animation cost, all of it: one small struct PER ENABLED CLIP
    // per skinned node. No matrix decompositions, no vertices, no uploads — the
    // sampling, the blending and the FK all happen inside the engine's threaded
    // update.
    //
    // TWO SOURCES, ONE PUSH (AVATAR_LOCOMOTION_SPEC Stage 5). An ordinary
    // animated node plays its ONE active authored clip at the scene clock. An
    // AVATAR plays the N weighted clips its locomotion state machine published
    // this step — a blend space's bracketing pair, doubled during a cross-fade —
    // each at its OWN absolute time. The locomotion source wins where it exists,
    // because a node carrying a locomotion component is being driven by the
    // machine and not by the transport.
    //
    // AND IT IS A PURE TRANSLATION, WHICH IS A REQUIREMENT AND NOT A STYLE
    // (§3.2a). Nothing about the machine is cached here: not the phase, not the
    // blend clock, not "which state am I in". `evacuateEngineObjects` releases
    // EVERY entry when the other page takes the shared document's graph
    // (releaseEntry ends with `e = Entry();`), so anything kept mirror-side is
    // destroyed on every editor<->player toggle and would come back reset. The
    // document advances the per-avatar clock and publishes ABSOLUTE times; this
    // function reads them and nothing else (§13 R1 — there is deliberately no
    // addTime on the boundary).
    //
    // THE LATCH IS GENERALISED TO N, and the old comment here ("pushed only when
    // the clip or the time actually moved") is now true of the AUTHORED path
    // only: under a blend space every weight and every time moves every frame,
    // so an avatar pushes every frame by construction. That is fine — the push
    // is a small struct array — and the latch still earns its keep twice: a
    // paused or still authored animation costs one implicitly-shared QString
    // compare per node and no engine call, and no std::string is built at all on
    // a frame whose push is skipped.
    if (!mSource) return;
    const float t = mSource->animationTime();
    for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
        Entry &e = *it;
        if (!e.gpuSkinned || !e.docNode) continue;
        // A FOLLOWER renders from the master's instance: it has no animation
        // state of its own, and the engine refuses clip calls on it by design
        // (AVATAR_RIG_PERF_SPEC §3.3). This is the row §1 removes — one push per
        // CHARACTER instead of one per piece.
        if (e.shareMaster) continue;
        attachClipsFor(e);
        if (e.clipMap.isEmpty()) continue;

        // ---- 1. what should be playing on this node, this frame ------------
        mClipPushScratch.clear();

        // (a) LOCOMOTION. The component lives on the avatar WRAPPER and the
        // skinned mesh is a descendant of it, so the lookup walks up — the same
        // direction clipHostOf walks, for the same reason.
        //
        // GATED ON THE DOCUMENT'S PLAY FLAG, and that is the whole gate: the
        // machine only advances inside the physics step (Environment::
        // updateAvatarMovement), so after Stop it still holds the last weight
        // set it published. Without this the Avatar page's transport would
        // never get its character back — the mirror would keep pushing a
        // frozen mid-stride blend over whatever clip the user scrubbed.
        // `Scene::playing` is the right flag rather than a mirror-side one:
        // PlayBack is the one place BOTH play paths pass through (editor
        // play-in-place and the player view) and it is what sets it, a PAUSED
        // scene deliberately stays "playing" so the pose holds, and it is
        // document state — so this is still a pure translation.
        const iris::AvatarLocomotion *loco =
            mSource->isPlaying() ? locomotionHostOf(e.docNode) : nullptr;
        if (loco) {
            float total = 0.0f;
            for (const auto &w : loco->weights()) {
                // A zero-weight sample is a real, ordinary output: a three-sample
                // blend space names all three clips every step and brackets two
                // of them. Leaving it out of the array DISABLES it engine-side
                // ("clips the array does not name are disabled"), which is one
                // fewer animation for the engine to sample and is exactly what a
                // zero weight means.
                if (w.weight <= 1e-6f) continue;
                const auto engineName = e.clipNameMap.constFind(w.clip);
                // A clip the machine names that this rig does not carry. Not
                // fatal and not worth a per-frame warning: the state machine
                // degrades on missing roles by design, and the remaining
                // weights still describe a pose.
                if (engineName == e.clipNameMap.constEnd()) continue;
                Entry::ClipPush p;
                p.name = engineName.value();
                p.time = w.time;
                p.weight = w.weight;
                p.looping = w.looping;
                mClipPushScratch.append(p);
                total += w.weight;
            }
            // R2 from the mirror's side. An empty set FREEZES the pose and an
            // all-zero set is REFUSED by the boundary, so if nothing the machine
            // named survived the mapping we fall through to the authored path
            // rather than pushing a set that means "stop rendering this
            // character correctly, silently".
            if (total <= 1e-6f) mClipPushScratch.clear();
        }

        // (b) the AUTHORED transport — one active animation at the scene clock.
        if (mClipPushScratch.isEmpty()) {
            iris::SceneNode *host = clipHostOf(e.docNode);
            iris::AnimationPtr active = host ? host->getAnimation() : iris::AnimationPtr();
            if (!active.isNull() && active->hasSkeletalAnimation()) {
                const auto mappedId = e.clipMap.constFind(active.data());
                if (mappedId != e.clipMap.constEnd()) {
                    const auto mapped = e.clipIdMap.constFind(mappedId.value());
                    if (mapped != e.clipIdMap.constEnd()) {
                        Entry::ClipPush p;
                        p.name = mapped.value();
                        // ABSOLUTE time, always — the document owns the clock and
                        // the engine does its own wrap (fmod when looping, clamp
                        // when not), which is exactly what
                        // Animation::getSampleTime does document-side.
                        p.time = t;
                        p.weight = 1.0f;
                        p.looping = active->getLooping();
                        mClipPushScratch.append(p);
                    }
                }
            }
        }

        // ---- 2. nothing to play ---------------------------------------------
        //
        // A node with clips attached but none active keeps its BIND pose: with
        // no active animation SkeletonInstance::update() does not even reset to
        // pose, so "no clip" is a frozen pose by construction, and the pose it
        // is frozen at is the one the rig was created with.
        if (mClipPushScratch.isEmpty()) {
            if (!e.lastClipPush.isEmpty()) {
                ++mClipStatePushes;
                mTarget->setClipStates(e.node, nullptr, 0);
                e.lastClipPush.clear();
            }
            continue;
        }

        // ---- 3. the latch, over the whole set -------------------------------
        bool changed = mClipPushScratch.size() != e.lastClipPush.size();
        for (int i = 0; !changed && i < mClipPushScratch.size(); ++i) {
            const Entry::ClipPush &a = mClipPushScratch[i];
            const Entry::ClipPush &b = e.lastClipPush[i];
            // The float compares stay FUZZY, as they were: a paused authored
            // animation must keep skipping its push, and a weight that moved by
            // one ulp is not a pose change.
            changed = a.name != b.name || a.looping != b.looping
                      || !qFuzzyCompare(a.time + 1.0f, b.time + 1.0f)
                      || !qFuzzyCompare(a.weight + 1.0f, b.weight + 1.0f);
        }
        if (!changed) continue;

        // ---- 4. the push -----------------------------------------------------
        mClipStateScratch.resize(size_t(mClipPushScratch.size()));
        for (int i = 0; i < mClipPushScratch.size(); ++i) {
            const Entry::ClipPush &p = mClipPushScratch[i];
            ClipState &s = mClipStateScratch[size_t(i)];
            // assign() into the reused string rather than a fresh std::string:
            // this array is rewritten for every avatar on every frame.
            const QByteArray utf8 = p.name.toUtf8();
            s.name.assign(utf8.constData(), size_t(utf8.size()));
            s.enabled = true;
            s.time = p.time;
            // RAW INTENT. The weights are NOT normalized here and must not be:
            // the backend normalizes them PER BONE from the clips' coverage, so
            // a bone only one clip animates gets all of that clip at any split
            // (Engine.h:207, proved by tests/skeletal G5). Summing them to 1
            // here would be a second, wrong normalization.
            s.weight = p.weight;
            s.looping = p.looping;
        }
        ++mClipStatePushes;
        if (mTarget->setClipStates(e.node, mClipStateScratch.data(), mClipStateScratch.size()))
            e.lastClipPush = mClipPushScratch;
    }
}

void SceneMirror::invalidateEnvironment()
{
    // Only the "already pushed" latches: the LAST-value members stay, so a
    // re-push that lands on the same values is still cheap where the engine
    // setter is idempotent, and correct where it is not.
    mAmbientPushed = false;
    mLastAmbientWasSky = false;
    mFogPushed = false;
    mGiPushed = false;
}

// CAMERA_LENS_SPEC §4/§5. Defined beside applyCamera, where the whole model is
// written out, and declared here because applyEnvironment builds the post
// description a beat earlier in the frame and must substitute the same values —
// see the comment at the call site for why doing it in only one of the two
// places rebuilds the compositor twice a frame.
static bool cameraOverridesAnything(const iris::CameraNodePtr &camera);
static void applyCameraPostFx(const iris::CameraNodePtr &camera, PostFxDesc &fx);

// THE LOOKS STACK, document -> engine (POST_LOOKS_SPEC.md §4.1).
//
// The document stores an ordered array of {id, enabled, params}; the engine
// takes an ordered vector of {kind, p[8]}. This is the whole translation, and
// it is the only place the two spellings meet.
//
// THE KIND MAP IS AN EXPLICIT SWITCH and not a cast, deliberately: the two
// enums are the same list today and the compiler would happily let them drift
// apart tomorrow. The document must not include the engine's header, so a
// static_assert cannot stand in for it — the switch is the seam, and a new look
// that forgets this line does not compile (-Wswitch).
static std::vector<LookDesc> resolveLooks(const QJsonArray &stack)
{
    std::vector<LookDesc> out;
    if (stack.isEmpty()) return out;   // the overwhelmingly common case
    const QJsonArray clean = iris::normalizeLookStack(stack);
    out.reserve(size_t(clean.size()));
    for (const QJsonValue &v : clean) {
        const QJsonObject entry = v.toObject();
        if (!entry.value(QStringLiteral("enabled")).toBool(true)) continue;
        const iris::LookDef *def = iris::lookDef(entry.value(QStringLiteral("id")).toString());
        if (!def) continue;
        LookDesc desc;
        switch (def->kind) {
        case iris::LookKind::Desaturate: desc.kind = LookKind::Desaturate; break;
        case iris::LookKind::GlassWarp:  desc.kind = LookKind::GlassWarp;  break;
        case iris::LookKind::RadialBlur: desc.kind = LookKind::RadialBlur; break;
        case iris::LookKind::OldMovie:   desc.kind = LookKind::OldMovie;   break;
        case iris::LookKind::Posterize:  desc.kind = LookKind::Posterize;  break;
        case iris::LookKind::Sharpen:    desc.kind = LookKind::Sharpen;    break;
        case iris::LookKind::FilmGrade:  desc.kind = LookKind::FilmGrade;  break;
        case iris::LookKind::Count:      continue;
        }
        iris::lookParamValues(*def, entry, desc.p);
        out.push_back(desc);
    }
    return out;
}

void SceneMirror::applyEnvironment(View *view, Engine *engine)
{
    if (!mSource || !view) return;
    // SHADOWS: ONE per-scene request (ENGINEERING_DEBT_SPEC.md item 4).
    //
    // The document has a per-light ShadowMapType and a per-light map size; the
    // engine has ONE filter and ONE atlas for the whole process. The reduction
    // is policy and stays here: the strongest (softest) filter and the largest
    // map any shadow-casting light asked for win, as accumulated by the last
    // sync(). The World panel's "Shadow Softness" (scene->shadowFilterTier,
    // POST_CHAIN_SPEC §9.3) and "Shadow Quality" (scene->shadowResolution,
    // VISUAL_PARITY item 2 option A) OVERRIDE that derivation outright —
    // including for scenes with no shadow caster yet, so the setting is what
    // the user asked for and not a function of the light list.
    //
    // What is NOT here any more is the read-before-write bookkeeping: three
    // hand-written "is the engine already showing this?" guards around two
    // global Engine setters, one per knob per policy branch. Scene::
    // setShadowSettings drops a request for what is already in force, which is
    // the same test in the one place that can see the global state.
    {
        ShadowDesc shadows;
        const int tier = mSource->shadowFilterTier;
        if (tier >= 0) {
            shadows.hasFilter = true;
            shadows.filter = tier >= 2 ? ShadowFilter::VerySoft
                           : tier == 1 ? ShadowFilter::Soft
                                       : ShadowFilter::Hard;
        } else if (mAnyShadowCaster) {
            // No caster and no override = no opinion: the engine's filter is
            // left alone rather than dragged back to a default.
            shadows.hasFilter = true;
            shadows.filter = mShadowFilter;
        }
        // 0 = no opinion, same reasoning. The engine rebuilds its shadow atlas
        // on a CHANGE, which is why "ask for what is already in force" has to
        // cost nothing.
        shadows.resolution = mSource->shadowResolution > 0
                                 ? unsigned(qBound(256, mSource->shadowResolution, 8192))
                                 : (mAnyShadowCaster ? mMaxShadowResolution : 0u);
        mTarget->setShadowSettings(shadows);
    }
        // The particle clock is NOT pushed from here any more: the HOST pushes
        // the document's simulated seconds x particleTimeScale as the engine's
        // frame delta every frame (EngineSceneViewport::syncFrame,
        // EnginePlayerScene::step — ENGINEERING_DEBT_SPEC A4.2), the one clock
        // physics and animation advance on too.
    if (engine) {
        // world.refreshShadows(): one re-render of every static shadow map per
        // bump, the giRefreshSerial shape exactly (a serial, not a bool: two
        // refreshes in one frame are still one re-render, and clearing is the
        // mirror's job rather than the caller's).
        if (mSource->shadowRefreshSerial != mShadowRefreshSerialSeen) {
            mShadowRefreshSerialSeen = mSource->shadowRefreshSerial;
            engine->refreshShadows();
        }
        // Shadow-map BUDGET (SHADOW_TOOLING_SPEC.md §4.1). Unlike the size,
        // there is nothing to derive from the lights here: the engine does that
        // itself, per frame, from the light list it is about to draw. This
        // pushes the CEILING only — the World Mode's tier value, or whatever
        // the scene pinned. 0 (Auto with no tier resolved) leaves the engine's
        // own default alone.
        const unsigned budget = mSource->shadowMapBudget > 0
                                    ? unsigned(qBound(2, mSource->shadowMapBudget, 16))
                                    : 0u;
        if (budget > 0 && engine->shadowMapBudget() != budget)
            engine->setShadowMapBudget(budget);
    }
    // Ambient. Historically the flat World-panel colour, twice (the engine
    // viewport used to hardcode the hemisphere — the panel no-op'd). With a sky
    // present and scene->ambientFromSky on (the default, VISUAL_PARITY item
    // 3b), the two hemisphere colours come from the SKY's own cosine-weighted
    // integrals instead, so a red sky reddens what it lights.
    {
        const QColor a = mSource->ambientColor;
        float sh[27] = { 0.0f };
        const bool fromSky = mHasSkyAmbient && mSource->ambientFromSky;
        if (fromSky) {
            // The World-panel colour becomes a per-channel GAIN on the sky's own
            // integral: white = the sky at full physical strength, black = no
            // ambient at all, and the document default (96,96,96 -> 0.376) lands
            // in the same brightness band the flat ambient used to occupy.
            // Pushing the raw integral instead would double the ambient of every
            // daylight scene — the sky is a full-hemisphere emitter and the flat
            // grey never was.
            // Owner-tuned (2026-09-03, two passes): 1.0 read too dark, 2.0
            // blew out once the same-day SSAO sky-darkening fix landed — 1.4
            // is the called midpoint. Ambient Color stays the artistic dial.
            const float kAmbientLift = 1.4f;
            const float gain[3] = { float(a.redF()) * kAmbientLift,
                                    float(a.greenF()) * kAmbientLift,
                                    float(a.blueF()) * kAmbientLift };
            for (int i = 0; i < 9; ++i)
                for (int c = 0; c < 3; ++c) sh[i * 3 + c] = mSkyAmbientSh[i * 3 + c] * gain[c];
        } else {
            // No sky (or Ambient From Sky off): the flat World-panel colour, the
            // way it always was. Scene::setAmbient converts it to the same SH
            // form — see the note there about the two scales HlmsPbs' own
            // ambient paths use.
            const Colour flat(a.redF(), a.greenF(), a.blueF(), 1.0f);
            if (!mAmbientPushed || mLastAmbientWasSky ||
                mLastFlatAmbient.r != flat.r || mLastFlatAmbient.g != flat.g ||
                mLastFlatAmbient.b != flat.b) {
                mTarget->setAmbient(flat, flat);
                mLastFlatAmbient = flat;
                mLastAmbientWasSky = false;
                mAmbientPushed = true;
            }
        }
        // Push on CHANGE only. The coefficients feed a pass buffer that HlmsPbs
        // rebuilds per pass anyway, but setSphericalHarmonics also re-decides the
        // ambient shader variant, so a per-frame push of an unchanged value was
        // asking a shader/root-layout question every frame for nothing.
        if (fromSky) {
            bool changed = !mAmbientPushed || !mLastAmbientWasSky;
            for (int i = 0; !changed && i < 27; ++i) changed = sh[i] != mLastAmbientSh[i];
            if (changed) {
                mTarget->setAmbientSh(sh);
                std::memcpy(mLastAmbientSh, sh, sizeof(sh));
                mLastAmbientWasSky = true;
                mAmbientPushed = true;
            }
        }
    }
    // World-panel Enable Shadows (used to be hardcoded on).
    if (view->shadows() != mSource->shadowEnabled)
        view->setShadows(mSource->shadowEnabled);
    // World-panel Anti-Aliasing: per-scene MSAA sample count. Safe to push per
    // frame — the engine ignores a repeat of the value already REQUESTED (the
    // achieved count may be clamped lower by the driver, so comparing against
    // view->sampleCount() here would rebuild the target every frame).
    //
    // ON-SCREEN ONLY (POST_CHAIN_SPEC.md §7.3). Offscreen views — thumbnails,
    // material previews, the asset and avatar viewers, screenshots and every
    // pixel suite — stay at 1x so their readbacks are exact and reproducible.
    // Pushing the scene's count to them was a latent inconsistency: harmless
    // while scenes defaulted to 1x, and a whole-suite re-baseline the moment a
    // World Mode set 4x.
    if (!view->isOffscreen())
        view->setSampleCount(unsigned(qBound(1, mSource->antiAliasing, 16)));
    // World panel post-processing chain (POST_CHAIN_SPEC.md §§3-7). Safe to push
    // per frame: the engine ignores a repeat of the value already set, and only
    // a change to an ENABLE flag rebuilds a workspace. Offscreen views (this
    // includes thumbnails, previews and every pixel suite) discard it inside the
    // engine, in one place — the host does not have to remember to.
    {
        PostFxDesc fx;
        fx.hdr            = mSource->hdrEnabled;
        fx.exposure       = mSource->exposure;
        fx.exposureMin    = mSource->exposureMin;
        fx.exposureMax    = mSource->exposureMax;
        fx.bloom          = mSource->bloomEnabled;
        fx.bloomThreshold = mSource->bloomThreshold;
        fx.bloomKnee      = mSource->bloomKnee;
        fx.ssao           = mSource->ssaoEnabled;
        fx.ssaoScale      = mSource->ssaoScale;
        fx.ssaoPower      = mSource->ssaoPower;
        fx.ssaoRadius     = mSource->ssaoRadius;
        fx.smaaPreset     = mSource->smaaPreset;
        fx.ssr            = mSource->ssrMode;
        // Refraction "Auto" (the recommended default): the second scene pass and
        // its full-res copy only enter the graph while the scene actually holds a
        // refractive material, so the cost when unused is exactly zero. The flag
        // is accumulated by sync() the same way mAnyShadowCaster is.
        fx.refractions    = mSource->refractionsMode == 2 ||
                            (mSource->refractionsMode == 1 && mAnyRefractive);
        // Distortion, resolved the same way (POST_LOOKS_SPEC §5.3): 0 off,
        // 1 auto (only while the scene holds a distortion material — the
        // recommended default), 2 always.
        fx.distortion     = mSource->distortionMode == 2 ||
                            (mSource->distortionMode == 1 && mAnyDistortion);
        fx.distortionStrength = mSource->distortionStrength;
        // THE LOOKS STACK (POST_LOOKS_SPEC §4.1). The document's array, in its
        // own order, minus the entries the user switched off — a disabled look
        // stays in the document and out of the graph, which is what makes the
        // panel's toggle free rather than a destructive edit.
        fx.looks = resolveLooks(mSource->looks);
        // THE DRIVING CAMERA'S OWN LOOK, layered over the world's
        // (CAMERA_LENS_SPEC §4/§5 — applyCameraPostFx documents the model).
        //
        // IT HAS TO HAPPEN HERE AS WELL AS IN applyCamera, and the reason is
        // performance rather than taste: hosts call applyEnvironment and then
        // applyCamera every frame, and an enable-flag difference between the
        // two descriptions is a WORKSPACE REBUILD. Pushing the world's flags
        // here and the camera's a moment later would rebuild the chain TWICE
        // PER FRAME for as long as a camera with a bloom override was driving.
        // Substituting here makes the steady state one stable description that
        // the engine's own "same value is free" check drops on arrival, and
        // applyCamera's push then only ever does anything on the frame the
        // camera actually changed.
        //
        // The record is a frame old (applyCamera writes it after this runs), so
        // a CUT grades one frame late — 16 ms, and applyCamera corrects it in
        // the same frame anyway. A view seen for the FIRST time has no record
        // at all and gets the world's description, which is exactly right: the
        // camera has not been applied to it yet.
        // THE WORLD'S OWN DESCRIPTION, before any camera is layered over it.
        // The picture-in-picture inset needs exactly this and not what the view
        // ends up with: the inset is a DIFFERENT camera's shot, so inheriting
        // the MAIN view's camera grade would hand the pipped camera the driving
        // camera's exposure (applyPip layers the pipped camera over this).
        mWorldPostFx = fx;
        if (const iris::CameraNodePtr driving = drivingCameraFor(view))
            if (cameraOverridesAnything(driving)) applyCameraPostFx(driving, fx);
        view->setPostFx(fx);
    }
    // Fog panel: exponential distance fog (+ optional height layer) on lit
    // surfaces; the engine keeps unlit overlays and the sky unfogged, like the
    // legacy renderer. Cheap per-frame push WHILE THE STATE HOLDS — but the
    // enabled edge builds/destroys the scene's atmosphere and swaps shader
    // variants, so push only on change.
    {
        FogDesc fog;
        fog.enabled = mSource->fogEnabled;
        const QColor f = mSource->fogColor;
        fog.colour = Colour(f.redF(), f.greenF(), f.blueF(), 1.0f);
        fog.density = mSource->fogDensity;
        fog.heightDensity = mSource->fogHeightDensity;
        fog.heightFalloff = mSource->fogHeightFalloff;
        fog.heightLevel = mSource->fogHeightLevel;
        fog.breakMinBrightness = mSource->fogBreakMinBrightness;
        fog.breakFalloff = mSource->fogBreakFalloff;
        const bool changed =
            !mFogPushed || mLastFog.enabled != fog.enabled ||
            mLastFog.colour.r != fog.colour.r || mLastFog.colour.g != fog.colour.g ||
            mLastFog.colour.b != fog.colour.b || mLastFog.density != fog.density ||
            mLastFog.heightDensity != fog.heightDensity ||
            mLastFog.heightFalloff != fog.heightFalloff ||
            mLastFog.heightLevel != fog.heightLevel ||
            mLastFog.breakMinBrightness != fog.breakMinBrightness ||
            mLastFog.breakFalloff != fog.breakFalloff;
        if (changed) { mTarget->setFog(fog); mLastFog = fog; mFogPushed = true; }
    }
    // Global Illumination panel. setGlobalIllumination re-traces, so like fog it is
    // pushed on CHANGE only (the per-frame compare is the debounce) — and it also
    // re-traces when the driving light itself moved — Instant
    // Radiosity solves in milliseconds at editor quality, per GI_SPEC.md.
    {
        GiParams gi;
        switch (mSource->giMode) {
        case iris::GiMode::INSTANT_RADIOSITY: gi.mode = GiMode::InstantRadiosity; break;
        case iris::GiMode::VCT:               gi.mode = GiMode::Vct; break;
        case iris::GiMode::VCT_PCC_HYBRID:    gi.mode = GiMode::VctPccHybrid; break;
        case iris::GiMode::OFF: default:      gi.mode = GiMode::Off; break;
        }
        switch (mSource->giQuality) {
        case iris::GiQuality::LOW:             gi.quality = GiQuality::Low; break;
        case iris::GiQuality::HIGH:            gi.quality = GiQuality::High; break;
        case iris::GiQuality::MEDIUM: default: gi.quality = GiQuality::Medium; break;
        }
        gi.boundsMin = toVec3(mSource->giBoundsMin);
        gi.boundsMax = toVec3(mSource->giBoundsMax);
        gi.numBounces = mSource->giNumBounces;
        gi.pccProbesX = qBound(1, qRound(mSource->giPccGrid.x()), 8);
        gi.pccProbesY = qBound(1, qRound(mSource->giPccGrid.y()), 8);
        gi.pccProbesZ = qBound(1, qRound(mSource->giPccGrid.z()), 8);
        // Probe-capture knobs (REFLECTIONS_ADOPTION_SPEC P3). The two toggles
        // travel as the document's tri-state int; anything outside -1..1 is a
        // corrupt document and reads as Auto.
        const auto toggle = [](int v) {
            return v == 0 ? GiToggle::Off : (v == 1 ? GiToggle::On : GiToggle::Auto);
        };
        gi.probeHdr = toggle(mSource->giProbeHdr);
        gi.probeShadows = toggle(mSource->giProbeShadows);
        gi.probeOverlap = mSource->giProbeOverlap;
        gi.probeSnapDeviation = mSource->giProbeSnapDeviation;
        gi.probeSnapSidesMin = mSource->giProbeSnapSidesMin;
        gi.probeSnapSidesMax = mSource->giProbeSnapSidesMax;
        gi.updateBudget = qMax(0, mSource->giUpdateBudget);        // FIX WAVE B1
        gi.dynamicProbes = qBound(0, mSource->giDynamicProbes, 8);  // Epic's column
        gi.rayMarchStepScale = qMax(1.0f, mSource->giRayMarchStepScale);   // B5
        // DDGI (GI_UNIFIED_SPEC.md §4 P1): the same tri-state travel as the
        // probe toggles, plus our own intensity scalar. Both ride the CHANGE
        // debounce below like every other GI field — the intensity included,
        // deliberately: it is a const-buffer write in the engine, but the only
        // channel into the engine is setGlobalIllumination, and a knob that
        // rebuilt sometimes and not others would be worse than one that always
        // does (recorded as a P2 tuning item, with the panel).
        gi.ddgi = toggle(mSource->giDdgi);
        gi.ddgiIntensity = qBound(0.0f, mSource->giDdgiIntensity, 64.0f);
        gi.ddgiAmbient = qBound(0.0f, mSource->giDdgiAmbient, 8.0f);
        // The probe source (rayon2 S3): -1 auto, 0 voxel, 1 raster.
        gi.ddgiSource = mSource->giDdgiSource == 0 ? GiSource::Voxel
                      : (mSource->giDdgiSource == 1 ? GiSource::Raster : GiSource::Auto);
        iris::LightNode *driver = gi.mode == GiMode::InstantRadiosity ? resolveGiLight() : nullptr;
        gi.irLight = driver ? engineNode(driver) : 0;
        // What a refresh should track depends on the mode: IR re-traces from
        // ONE driving light, so only that light's transform matters; VCT
        // injects EVERY light into the voxel volume, so any light moving (or
        // appearing/dying) goes stale until a re-voxelize.
        //
        // The signature is a HASH of local TRS up each light's parent chain,
        // not a product of world matrices (audit F8). The old form called
        // `getGlobalTransform()` — Ogre's `_getFullTransformUpdated`, which
        // recurses to the root and recomposes the transform at every level,
        // unconditionally — once per light per frame, and then multiplied the
        // results into a Mat4. Both halves were pure overhead for a question
        // that is only ever "is this the same as last frame?".
        // (It is also STRICTLY more sensitive than what it replaced: a product
        // of world matrices is blind to two lights swapping pure-translation
        // transforms, because translation matrices commute. A per-light hash
        // folded in scene order is not.)
        quint64 lightSig = 0;
        if (driver) {
            lightSig = worldTrsSignature(driver->graphNode());
        } else if (gi.mode == GiMode::Vct || gi.mode == GiMode::VctPccHybrid) {
            mGiChainMemo.clear();          // capacity kept; contents are per call
            Hasher h;
            for (const auto &l : mSource->lights)
                if (!l.isNull()) h << worldTrsSignatureMemo(l->graphNode(), mGiChainMemo);
            lightSig = h.h;
        }
        // ---- RE-FIT ON EXIT (LIGHTING_FIX fix 2) ---------------------------
        //
        // A light moving is not the only thing that invalidates a GI solve: an
        // OBJECT leaving the lit volume does too, and it was the one nothing
        // watched. Raise a cube above the auto-fitted volume and it kept the
        // lighting it had at the old height for ever — until the user happened
        // to nudge a light, at which point the volume re-fitted and everything
        // "mysteriously" fixed itself. That workaround is the bug report.
        //
        // The engine answers the question as a SIGNATURE, not a flag, precisely
        // so it can ride this machinery unchanged: it is 0 while everything is
        // inside the volume, and while an object is outside it changes on every
        // frame the object moves. Folded in beside the light signature, that
        // gives the same two behaviours the debounce already guarantees for a
        // dragged light — a continuous drag re-arms the window every frame and
        // costs no rebuilds, and letting go costs exactly one.
        //
        // (Not folded in for Instant Radiosity: its signature is the ONE driving
        // light by design, and IR's area of interest is re-derived from the same
        // bounds on every re-trace anyway.)
        const bool vctLike = gi.mode == GiMode::Vct || gi.mode == GiMode::VctPccHybrid;
        // The raw light term is kept so the post-refresh re-read below can
        // recombine it with a FRESH escape term rather than re-hashing an
        // already-combined value (which would never match the next frame's).
        const quint64 lightSigRaw = lightSig;
        // ---- THE MOVEMENT TERM (FIX WAVE B3) --------------------------------
        //
        // Third term, same shape and the same debounce as the other two: a
        // quantized hash of every GI item's world AABB. Before it, MOVING
        // geometry changed nothing at all — the panel's own Refresh tooltip said
        // so out loud ("moving objects does not do this automatically") — because
        // the only cheap answer available was "re-solve every frame of the drag".
        // There are cheap answers now: the probes covering the mover re-capture
        // on their next turn in the engine's per-frame budget, and the lights are
        // re-injected into the voxels on the kGiLightOnlyEveryN cadence. So
        // geometry gets the treatment lights already had: the cheap paths during
        // the gesture, exactly one full re-solve when it stops.
        //
        // Folded into the SAME signature rather than given its own gate, so that
        // moving a light and moving a box during one drag still cost one
        // re-solve between them rather than two.
        const auto combine = [&](quint64 light, quint64 escape, quint64 geometry) {
            if (!vctLike) return light;
            Hasher h; h << light << escape << geometry; return h.h;
        };
        if (vctLike)
            lightSig = combine(lightSigRaw, mTarget->giEscapeSignature(),
                               mTarget->giGeometrySignature());
        // The engine's half of the signature is read from DERIVED world AABBs,
        // and those are only correct once something has run updateSceneGraph —
        // which, on the very first sync, is the GI build itself. Reading it
        // before the push therefore hashes the items at their birth transforms
        // and the next frame looks like a scene-wide move: one spurious
        // re-solve, 15 frames after every arm (caught by gi.coalesce's "20 idle
        // frames cost nothing at all"). So the push adopts the signature AFTER
        // it pushes, exactly as the two refresh branches below already do.
        const auto readEngineSignature = [&]() {
            return combine(lightSigRaw, mTarget->giEscapeSignature(),
                           mTarget->giGeometrySignature());
        };
        // Compared BY VALUE (GiParams::operator==, beside the struct — every
        // field setGlobalIllumination reads is in it, so a new field cannot fall
        // behind the comparison the way a lambda one file away did).
        //
        // A dynamicProbes-ONLY change takes the cheap path (code review
        // 2026-09-10): the full push re-voxelizes and re-captures every probe,
        // and the Advanced slider emits per drag tick.
        GiParams onlyDynamic = gi;
        onlyDynamic.dynamicProbes = mLastGi.dynamicProbes;
        if (mGiPushed && gi != mLastGi && onlyDynamic == mLastGi) {
            mTarget->setGiDynamicProbes(gi.dynamicProbes);
            mLastGi.dynamicProbes = gi.dynamicProbes;
        }
        if (!mGiPushed || gi != mLastGi) {
            mTarget->setGlobalIllumination(gi);
            mLastGi = gi;
            mGiLightSignature = readEngineSignature();
            mGiPushed = true;
            mGiPendingRefresh = false;
            mGiStableFrames = 0;
            mGiRefreshSerialSeen = mSource->giRefreshSerial;
            ++mGiPushCount;
        } else if (gi.mode != GiMode::Off) {
            // ---- REBUILD COALESCING (REFLECTIONS_ADOPTION_SPEC.md §5 / P2) ----
            //
            // A full GI refresh is a teardown plus a re-voxelize plus, in the
            // hybrid, every probe re-rendered twice (216 face renders at the
            // shipped 18-probe grid). Doing it on the frame a light MOVES means
            // doing it on every frame of a drag, synchronously inside mirror
            // sync. The old code did exactly that — the branch's own comment
            // knew, and the only thing saving the frame rate was that lights
            // are usually still.
            //
            // So a changed light signature no longer refreshes. It ARMS a
            // pending refresh and resets a stability counter; the expensive
            // rebuild fires once the signature has held still for
            // kGiStableFrames frames or kGiStableMs milliseconds, whichever
            // comes first. During the wait the CHEAP path runs every
            // kGiLightOnlyEveryN frames — re-inject the lights into the voxels
            // that are already there — so bounced light follows the light being
            // dragged instead of freezing until the mouse is released. Probes
            // deliberately do not update on that path; they come back at the
            // stability fire.
            //
            // Both a frame count AND a clock, because neither alone is right:
            // frames alone make the delay depend on how fast the scene renders
            // (and tests that step frames by hand would never fire), a clock
            // alone makes a stepped test depend on wall time. Whichever arrives
            // first wins, so a headless test that pumps 15 frames instantly
            // still gets its refresh.
            const bool sigChanged = lightSig != mGiLightSignature;
            // world.refreshGi() / the panel's Refresh button (P1d): an explicit
            // demand, so it does NOT wait for the stability window.
            const bool explicitRefresh = mSource->giRefreshSerial != mGiRefreshSerialSeen;

            if (sigChanged) {
                if (mSource->giUpdateBudget > 0) {
                    // The remembered signature is only advanced while the budget
                    // is above zero, exactly as it was only advanced while Auto
                    // Refresh was ON: with GI paused the mirror is not tracking
                    // the scene at all, so un-pausing must notice the moves that
                    // happened meanwhile rather than adopting them silently.
                    mGiLightSignature = lightSig;
                    // Both gates measure STABILITY, so both restart on every
                    // change: a drag that keeps changing the signature never
                    // satisfies either, which is the whole point. (Arming the
                    // clock once instead would fire a full re-solve mid-drag as
                    // soon as the drag outlasted 250 ms — the exact cost this
                    // phase exists to remove, just less often.)
                    mGiPendingTimer.restart();
                    if (!mGiPendingRefresh) mGiFramesSinceLightOnly = 0;
                    mGiPendingRefresh = true;
                    mGiStableFrames = 0;
                }
            } else if (mGiPendingRefresh) {
                ++mGiStableFrames;
            }

            // A re-solve RE-FITS the volume, so the escape term it may have been
            // armed by is 0 again the moment it returns. Re-reading the
            // signature after the rebuild and adopting it is what keeps "move a
            // cube out of the volume" costing ONE re-solve instead of two (the
            // second being the signature changing back).
            const auto adoptSignature = [&]() {
                if (vctLike) mGiLightSignature = readEngineSignature();
            };
            if (explicitRefresh) {
                mGiRefreshSerialSeen = mSource->giRefreshSerial;
                mGiPendingRefresh = false;
                mGiStableFrames = 0;
                mTarget->refreshGlobalIllumination();
                ++mGiRefreshCount;
                adoptSignature();
            } else if (mGiPendingRefresh &&
                       (mGiStableFrames >= kGiStableFrames ||
                        mGiPendingTimer.elapsed() >= kGiStableMs)) {
                mGiPendingRefresh = false;
                mGiStableFrames = 0;
                mTarget->refreshGlobalIllumination();
                ++mGiRefreshCount;
                adoptSignature();
            } else if (mGiPendingRefresh) {
                // Still moving: the cheap path, rate-limited.
                if (++mGiFramesSinceLightOnly >= kGiLightOnlyEveryN) {
                    mGiFramesSinceLightOnly = 0;
                    if (mTarget->refreshGiLighting()) ++mGiLightRefreshCount;
                }
            }
        }
    }
    // Planar reflections (PLANAR_REFLECTIONS_SPEC.md §6). Same discipline as GI:
    // the engine's setter is idempotent, but a CHANGE rebuilds render targets
    // and recompiles PBR shaders, so it is pushed every frame only because
    // pushing an unchanged value is free.
    {
        PlanarReflectionParams pr;
        // A negative budget is "never set" (a Custom-mode scene, or a document
        // written before the feature): off. Every path that applies a world mode
        // writes a concrete number here first — this file is IrisGL and cannot
        // see the mode table, which is exactly why the invariant exists.
        pr.budget = mSource->planarReflectionBudget < 0 ? 0
                                                        : qBound(0, mSource->planarReflectionBudget, 8);
        // Resolution and shadows follow the budget unless the scene pins them.
        // This IS the mode table's reflection row, expressed where the mirror
        // can reach it: Epic's budget of 2 gets 1024 + shadows, High's 1 gets
        // 512 and no shadows. Two extra world-mode rows would say the same
        // thing and give the user two more dials to get wrong.
        const int autoRes = pr.budget >= 2 ? 1024 : 512;
        pr.resolution = mSource->planarReflectionResolution > 0
                            ? unsigned(qBound(256, mSource->planarReflectionResolution, 2048))
                            : unsigned(autoRes);
        pr.shadows = mSource->planarReflectionShadows >= 0
                         ? mSource->planarReflectionShadows != 0
                         : pr.budget >= 2;
        // Glossy floors need the mip chain (the shader samples at
        // roughness * numMips); without it every reflector is a perfect mirror.
        pr.mipmaps = true;
        pr.accurateLighting = true;
        // The reflection's clear colour is the view's, so a mirror showing
        // "nothing" shows the same nothing the viewport does.
        pr.background = view->background();
        mTarget->setPlanarReflections(pr);
    }
    // THE GI VOLUME OVERLAY GOES LAST, and it belongs HERE rather than in
    // sync() (LIGHTING_FIX fix 9): it draws what `Scene::giStatus()` reports,
    // and giStatus only reports this frame's answer after the GI push above.
    // Driven from sync() it was a frame behind — visibly so in gi.overlay,
    // where switching GI off left the box on screen for one more frame.
    syncGiVolume();
}

iris::LightNode *SceneMirror::resolveGiLight() const
{
    if (!mSource) return nullptr;
    if (!mSource->giLightGuid.isEmpty()) {
        auto it = mSource->lights.constFind(mSource->giLightGuid);
        if (it != mSource->lights.constEnd() && !it.value().isNull()) return it.value().data();
    }
    // QHash order is arbitrary: pick deterministically by creation order (nodeId).
    iris::LightNode *directional = nullptr, *any = nullptr;
    for (const auto &l : mSource->lights) {
        if (l.isNull()) continue;
        if (l->lightType == iris::LightType::Directional &&
            (!directional || l->nodeId < directional->nodeId)) directional = l.data();
        if (!any || l->nodeId < any->nodeId) any = l.data();
    }
    return directional ? directional : any;
}

namespace {
// ---------------------------------------------------------------------------
// THE lat-long convention. Ogre's SkyEquirectangular_ps.glsl is now the only
// thing that turns a sky image into directions, so everything that reasons
// about one — the reflection-cube resample, the ambient integral, the realistic
// bake — goes through these two functions. (The retired sky SPHERE used a
// different mapping: u ran the other way and started at -X, so the same image
// hung 90 degrees round and mirrored. Anything that compares a sky pixel to a
// world direction had to be re-baselined when the sphere went.)
//   u = (atan2(x, -z) + PI) / 2PI     -> image centre column looks down -Z
//   v = acos(y) / PI                  -> row 0 is the zenith
constexpr float kPi = 3.14159265358979f;

inline void equirectDir(float u, float v, float &x, float &y, float &z)
{
    const float phi = v * kPi;
    const float s = std::sin(phi);
    y = std::cos(phi);
    const float t = u * 2.0f * kPi - kPi;
    x =  s * std::sin(t);
    z = -s * std::cos(t);
}

inline void dirToEquirect(float x, float y, float z, float &u, float &v)
{
    v = std::acos(std::min(1.0f, std::max(-1.0f, y))) / kPi;
    u = (std::atan2(x, -z) + kPi) / (2.0f * kPi);
}

// sRGB byte -> linear float, table-driven: the SH integral touches every texel
// of a sky that can be 4096x2048, three channels, and std::pow dominated it.
const float *srgbTable()
{
    static float t[256];
    static const bool once = [] {
        for (int i = 0; i < 256; ++i) {
            const float c = i / 255.0f;
            t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return true;
    }();
    (void)once;
    return t;
}

// Cosine-convolved irradiance in 9 SH bands, in the basis and units
// Scene::setAmbientSh documents. Accumulate raw radiance moments
//     A_i = sum( radiance * b_i(dir) * dOmega )
// over the basis polynomials b = {1, y, z, x, xy, yz, 3z^2-1, zx, x^2-y^2},
// then fold in BOTH the SH normalisation k_i (twice: once for the projection,
// once for the reconstruction) and the Lambert convolution ratio
// A_l/pi = {1, 2/3, 1/4}. The result evaluates to E(n)/pi, a mean incident
// radiance — the same quantity the two hemisphere colours used to carry, so a
// sky that used to light a surface at brightness X still does.
struct ShAccum {
    double a[9][3] = {};

    void add(float x, float y, float z, double r, double g, double b, double w)
    {
        const double bi[9] = { 1.0, y, z, x, double(x) * y, double(y) * z,
                               3.0 * double(z) * z - 1.0, double(z) * x,
                               double(x) * x - double(y) * y };
        for (int i = 0; i < 9; ++i) {
            const double f = bi[i] * w;
            a[i][0] += r * f; a[i][1] += g * f; a[i][2] += b * f;
        }
    }

    void finish(float out[27]) const
    {
        // A_l/pi * k_i^2, per band.
        static const double k[9] = {
            0.0795774715,                                     // 1     * 1/(4pi)
            0.1591549431, 0.1591549431, 0.1591549431,         // y,z,x * 2/3 * 3/(4pi)
            0.2984155183, 0.2984155183,                       // xy,yz * 1/4 * 15/(4pi)
            0.0248679599,                                     // 3z^2-1* 1/4 * 5/(16pi)
            0.2984155183,                                     // zx
            0.0746038796                                      // x^2-y^2 * 1/4 * 15/(16pi)
        };
        for (int i = 0; i < 9; ++i)
            for (int c = 0; c < 3; ++c) out[i * 3 + c] = float(a[i][c] * k[i]);
    }
};
}  // namespace

bool SceneMirror::SkySource::operator==(const SkySource &o) const
{
    // Exact float equality, EXCEPT that two NaNs compare equal: these fields
    // come from document sliders, and a NaN that never equalled itself would
    // re-bake the sky on every single frame (the 64-bit hash this replaced was
    // blind to the distinction, so this keeps the old behaviour).
    const auto same = [](float a, float b) { return a == b || (a != a && b != b); };
    if (kind != o.kind) return false;
    switch (kind) {
    case Kind::None:     return true;
    case Kind::Equirect: return equirectPath == o.equirectPath;
    case Kind::Cubemap:  return cubeTexture == o.cubeTexture;
    case Kind::Gradient:
        return gradientTop == o.gradientTop && gradientMid == o.gradientMid &&
               gradientBot == o.gradientBot && same(gradientOffset, o.gradientOffset);
    case Kind::Realistic:
        return same(luminance, o.luminance) && same(reileigh, o.reileigh) &&
               same(mieCoefficient, o.mieCoefficient) &&
               same(mieDirectionalG, o.mieDirectionalG) && same(turbidity, o.turbidity) &&
               same(sunPosX, o.sunPosX) && same(sunPosY, o.sunPosY) &&
               same(sunPosZ, o.sunPosZ) &&
               bakeResolution == o.bakeResolution && hdr == o.hdr;
    }
    return false;
}

/// The document's sky fields, read into the value applySky compares. A skyless
/// scene (or a single-colour one, or a textured one with no texture loaded)
/// reads as Kind::None — which is also the initial state, so a fresh
/// single-colour scene never pushes a redundant "no sky" on its first frame.
SceneMirror::SkySource SceneMirror::skySourceOf(const iris::Scene &scene)
{
    SkySource src;
    if (scene.skyType == iris::SkyType::EQUIRECTANGULAR && scene.skyTexture) {
        src.kind = SkySource::Kind::Equirect;
        src.equirectPath = scene.skyTexture->source;
    } else if (scene.skyType == iris::SkyType::CUBEMAP && scene.skyTexture &&
               scene.skyTexture->isCubeMap()) {
        src.kind = SkySource::Kind::Cubemap;
        src.cubeTexture = scene.skyTexture.data();
    } else if (scene.skyType == iris::SkyType::GRADIENT) {
        src.kind = SkySource::Kind::Gradient;
        src.gradientTop = scene.gradientTop;
        src.gradientMid = scene.gradientMid;
        src.gradientBot = scene.gradientBot;
        src.gradientOffset = scene.gradientOffset;
    } else if (scene.skyType == iris::SkyType::REALISTIC) {
        const iris::SkyRealistic &r = scene.skyRealistic;
        src.kind = SkySource::Kind::Realistic;
        src.luminance = r.luminance;
        src.reileigh = r.reileigh;
        src.mieCoefficient = r.mieCoefficient;
        src.mieDirectionalG = r.mieDirectionalG;
        src.turbidity = r.turbidity;
        src.sunPosX = r.sunPosX;
        src.sunPosY = r.sunPosY;
        src.sunPosZ = r.sunPosZ;
        src.bakeResolution = scene.skyBakeResolution;
        src.hdr = scene.hdrEnabled;
    }
    return src;
}

// BUILD THE DESCRIPTION, COMPARE, PUSH (ENGINEERING_DEBT_SPEC.md item 4).
//
// Two comparisons, and they answer different questions:
//   * SkySource — "is the sky made of the same things?" Its answer decides
//     whether the CPU BAKE runs (a Preetham evaluation, an equirect->cubemap
//     resample, an SH integral over every texel), which is the expensive half
//     and the reason this comparison exists at all.
//   * SkyDesc — "is this the sky the engine already has?" It is made of
//     texture ids, which only exist after the bake, and the ENGINE owns it:
//     Scene::setSky drops a description equal to the live one, per half, so
//     the push below is free every frame it changes nothing.
//
// The bakes stay HERE rather than moving below the boundary with the
// description: they need an image decoder and a Preetham evaluator, and the
// engine layer has neither by construction (Types.h: no Qt, no image formats).
void SceneMirror::applySky(View *view)
{
    if (!mSource || !view) return;
    const SkySource src = skySourceOf(*mSource);
    if (src != mSkySource) {
        // Debounce the realistic bake: a slider drag changes the 8 parameters on
        // every event, and the Preetham bake is per-pixel CPU math. Re-bake at
        // most every 150 ms — applySky re-reads the document next frame, so the
        // final value always lands once the slider settles.
        if (src.kind == SkySource::Kind::Realistic &&
            mSkySource.kind == SkySource::Kind::Realistic &&
            mRealisticBakeTimer.isValid() && mRealisticBakeTimer.elapsed() < 150)
            return;
        mSkySource = src;
        mSkyTexture = 0;
        mReclaimPending = true;
        for (TextureId &t : mSkyFaceTextures)  { if (t) mTarget->destroyTexture(t); t = 0; }
        for (TextureId &t : mReflFaceTextures) { if (t) mTarget->destroyTexture(t); t = 0; }
        // The ambient integral belongs to the sky that is about to be built:
        // drop the old one first so a failed build cannot leave a stale colour.
        clearSkyAmbient();
        // Default = no sky and NO OPINION about reflections; every failure path
        // below simply leaves it that way, which is what the old code spelled
        // out as setSky(NoSky, 0) in four places.
        mSkyDesc = SkyDesc();
        // Six freshly resampled faces become the description's reflection half.
        // A failed resample leaves `reflections` false — "no opinion" — so the
        // reflections already bound survive, exactly as they did when this was
        // a separate setSkyReflection() call that simply never happened.
        const auto attachReflection = [this](const QImage &image) {
            if (!buildSkyReflection(image)) return;
            mSkyDesc.reflections = true;
            for (int i = 0; i < 6; ++i) mSkyDesc.reflectionFaces[i] = mReflFaceTextures[i];
        };
        switch (src.kind) {
        case SkySource::Kind::Equirect: {
            const TextureId t = textureFor(mSource->skyTexture->source, true);
            mSkyTexture = t;   // held against reclaimUnused for as long as the sky stands
            if (t) {
                mSkyDesc.mode = SkyMode::Equirectangular;
                mSkyDesc.equirect = t;
                // Cubemap skies feed environment reflections (IBL); give equirect
                // skies the same by resampling the image into six small faces.
                attachReflection(QImage(mSource->skyTexture->source));
            }
            break;
        }
        case SkySource::Kind::Cubemap: {
            // The document keeps the six face images (+X,-X,+Y,-Y,+Z,-Z); upload them.
            const QImage *faces = mSource->skyTexture->cubeFaces();
            bool ok = faces != nullptr;
            for (int i = 0; ok && i < 6; ++i) {
                const QImage img = faces[i].convertToFormat(QImage::Format_RGBA8888);
                if (img.isNull()) { ok = false; break; }
                mSkyFaceTextures[i] = mTarget->createTexture(unsigned(img.width()), unsigned(img.height()), img.constBits(), true);
                if (!mSkyFaceTextures[i]) ok = false;
            }
            if (ok) {
                mSkyDesc.mode = SkyMode::Cubemap;
                for (int i = 0; i < 6; ++i) mSkyDesc.faces[i] = mSkyFaceTextures[i];
                // A cubemap sky never passes through buildSkyReflection (the
                // engine takes the faces straight): integrate them here so it
                // drives ambient like every other textured sky (item 3b).
                recordCubeAmbientSh(faces);
            }
            break;
        }
        case SkySource::Kind::Gradient: {
            // Legacy gradientsky.frag is a pure vertical 3-stop ramp: bake it into a
            // narrow equirect strip (row 0 = zenith) and reuse the equirect sky path.
            // The ramp itself is bakeGradientSky — shared with the glTF exporter,
            // which used to carry its own copy of it (re-audit F10).
            const QImage strip = iris::bakeGradientSky(mSource->gradientTop, mSource->gradientMid,
                                                       mSource->gradientBot, mSource->gradientOffset);
            mSkyFaceTextures[0] = strip.isNull() ? 0
                : mTarget->createTexture(unsigned(strip.width()), unsigned(strip.height()),
                                         strip.constBits(), true);
            if (mSkyFaceTextures[0]) {
                mSkyDesc.mode = SkyMode::Equirectangular;
                mSkyDesc.equirect = mSkyFaceTextures[0];
                attachReflection(strip);
            }
            break;
        }
        case SkySource::Kind::Realistic: {
            // Legacy realisticsky.frag (Preetham-style scattering), CPU-baked to
            // an equirect image and pushed through the same sky path as gradient.
            const int bakeW = mSource->skyBakeResolution >= 1024 ? 1024
                            : mSource->skyBakeResolution >= 512  ? 512 : 256;
            const QImage baked = bakeRealisticSky(mSource->skyRealistic, bakeW, bakeW / 2,
                                                  mSource->hdrEnabled);
            mRealisticBakeTimer.restart();
            if (!baked.isNull()) {
                mSkyFaceTextures[0] = mTarget->createTexture(unsigned(baked.width()), unsigned(baked.height()),
                                                             baked.constBits(), true);
                if (mSkyFaceTextures[0]) {
                    mSkyDesc.mode = SkyMode::Equirectangular;
                    mSkyDesc.equirect = mSkyFaceTextures[0];
                    attachReflection(baked);
                }
            }
            break;
        }
        case SkySource::Kind::None:
            break;
        }
    }
    // IDEMPOTENT (the assertion mirror.document_to_engine's sky-idempotency case
    // makes): an unchanged description costs one comparison inside the engine —
    // no upload, no cube rebuild, no IBL reconvolution, no workspace churn.
    // A push the engine REFUSED (a malformed cubemap, a missing face id) must
    // not be retried every frame — resync to what is in force so the retry
    // stops until the document changes (code review 2026-09-10).
    if (!mTarget->setSky(mSkyDesc)) mSkyDesc = mTarget->sky();
    if (mSource->skyType == iris::SkyType::SINGLE_COLOR) {
        const QColor c = mSource->skyColor;
        view->setBackground(Colour(c.redF(), c.greenF(), c.blueF(), 1.0f));
    }
}

namespace {
// Box-downsample an RGBA8888 image to at most `maxW` wide (halving until it
// fits, so every step is an exact 2x2 average). The equirect->cubemap resample
// below point-samples the result: without this a 4K sky is decimated ~30x and
// small bright features (a sun disc) alias into a crawling speckle as the sky
// changes (VISUAL_PARITY_SPEC item 3a).
QImage boxDownscaleTo(const QImage &src, int maxW)
{
    QImage img = src;
    while (img.width() > maxW && img.width() >= 2 && img.height() >= 2) {
        const int w = img.width() / 2, h = img.height() / 2;
        QImage out(w, h, QImage::Format_RGBA8888);
        for (int y = 0; y < h; ++y) {
            const unsigned char *r0 = img.constScanLine(y * 2);
            const unsigned char *r1 = img.constScanLine(y * 2 + 1);
            unsigned char *o = out.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const size_t a = size_t(x) * 8u;      // two source texels, 4 bytes each
                for (int c = 0; c < 4; ++c)
                    o[size_t(x) * 4u + c] = (unsigned char)((int(r0[a + c]) + int(r0[a + 4 + c]) +
                                                             int(r1[a + c]) + int(r1[a + 4 + c]) + 2) / 4);
            }
        }
        img = out;
    }
    return img;
}
} // namespace

void SceneMirror::clearSkyAmbient()
{
    mHasSkyAmbient = false;
    for (float &c : mSkyAmbientSh) c = 0.0f;
}

bool SceneMirror::integrateSkyAmbientSh(const QImage &equirect, float shOut[27])
{
    if (equirect.isNull()) return false;
    const QImage src = equirect.convertToFormat(QImage::Format_RGBA8888);
    const int W = src.width(), H = src.height();
    if (W <= 0 || H <= 0) return false;
    const float *lut = srgbTable();
    // Per-column longitude, hoisted: every row shares it.
    std::vector<float> sinT(static_cast<std::vector<float>::size_type>(W)),
                       cosT(static_cast<std::vector<float>::size_type>(W));
    for (int col = 0; col < W; ++col) {
        const float t = (col + 0.5f) / W * 2.0f * kPi - kPi;
        sinT[size_t(col)] = std::sin(t);
        cosT[size_t(col)] = std::cos(t);
    }
    ShAccum acc;
    for (int row = 0; row < H; ++row) {
        const float phi = (row + 0.5f) / H * kPi;
        const float sp = std::sin(phi), y = std::cos(phi);
        // Solid angle of one texel in this row: sin(phi) * dphi * dtheta.
        const double w = double(sp) * (kPi / H) * (2.0 * kPi / W);
        if (w <= 0.0) continue;
        const unsigned char *p = src.constScanLine(row);
        for (int col = 0; col < W; ++col) {
            const float x =  sp * sinT[size_t(col)];
            const float z = -sp * cosT[size_t(col)];
            acc.add(x, y, z, lut[p[size_t(col) * 4u + 0]], lut[p[size_t(col) * 4u + 1]],
                    lut[p[size_t(col) * 4u + 2]], w);
        }
    }
    acc.finish(shOut);
    return true;
}

void SceneMirror::recordCubeAmbientSh(const QImage faces[6])
{
    if (!faces) return;
    // Face order is +X,-X,+Y,-Y,+Z,-Z in WORLD axes (what SkyDesc::faces takes;
    // the engine converts to Ogre's left-handed cube itself). Same basis
    // vectors the equirect->faces resample below uses, so a cubemap sky and an
    // equirect sky of the same environment integrate to the same coefficients.
    // 1/len^3 is the cube-face texel's solid-angle factor.
    static const float ax[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    static const float rt[6][3] = {{0,0,-1},{0,0,1},{1,0,0},{1,0,0},{1,0,0},{-1,0,0}};
    static const float upv[6][3] = {{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
    const float *lut = srgbTable();
    ShAccum acc;
    bool any = false;
    for (int f = 0; f < 6; ++f) {
        // A face is uniform enough at 32x32 for an irradiance integral, and the
        // downscale is a box filter, so this is cheap and stable.
        const QImage img = boxDownscaleTo(faces[f].convertToFormat(QImage::Format_RGBA8888), 32);
        const int N = img.width(), M = img.height();
        if (N <= 0 || M <= 0) continue;
        any = true;
        const float *a = ax[f], *r = rt[f], *u = upv[f];
        const double texel = (2.0 / N) * (2.0 / M);
        for (int py = 0; py < M; ++py) {
            const float uv = 1.0f - 2.0f * (py + 0.5f) / M;
            const unsigned char *p = img.constScanLine(py);
            for (int px = 0; px < N; ++px) {
                const float ur = 2.0f * (px + 0.5f) / N - 1.0f;
                float dx = a[0] + r[0] * ur + u[0] * uv;
                float dy = a[1] + r[1] * ur + u[1] * uv;
                float dz = a[2] + r[2] * ur + u[2] * uv;
                const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (len < 1e-6f) continue;
                const double w = texel / (double(len) * len * len);
                dx /= len; dy /= len; dz /= len;
                acc.add(dx, dy, dz, lut[p[size_t(px) * 4u + 0]], lut[p[size_t(px) * 4u + 1]],
                        lut[p[size_t(px) * 4u + 2]], w);
            }
        }
    }
    if (!any) return;
    acc.finish(mSkyAmbientSh);
    mHasSkyAmbient = true;
}

bool SceneMirror::buildSkyReflection(const QImage &equirect)
{
    if (equirect.isNull()) return false;
    // The ambient integral runs on the FULL-resolution image (it is a mean; the
    // decimation below would bias it) before anything else touches it.
    if (integrateSkyAmbientSh(equirect, mSkyAmbientSh))
        mHasSkyAmbient = true;

    // The faces are only BUILT here; they reach the engine as the reflection
    // half of the SkyDesc applySky pushes (one description, one verb).
    TextureId ids[6] = { 0, 0, 0, 0, 0, 0 };
    if (!buildEquirectCubeFaces(equirect, ids)) {
        for (int i = 0; i < 6; ++i) if (ids[i]) mTarget->destroyTexture(ids[i]);
        return false;
    }
    for (int i = 0; i < 6; ++i) mReflFaceTextures[i] = ids[i];
    return true;
}

// THE SIX WORLD-AXIS FACES of an equirect panorama, as engine textures the
// caller owns. Factored out of buildSkyReflection so the per-material
// reflection override (ADDENDUM A-5) builds its cube from the SAME projection
// the sky uses — a second copy of this is how one of the two ends up mirrored
// or rotated against the other.
bool SceneMirror::buildEquirectCubeFaces(const QImage &equirect, TextureId ids[6])
{
    for (int i = 0; i < 6; ++i) ids[i] = 0;
    if (equirect.isNull() || !mTarget) return false;

    const int N = 128;   // reflection cube face size; the engine mips it further
    // Box-filter the source down to ~4 texels per face texel before sampling:
    // point-sampling a 4K equirect into 128^2 faces throws away 99.9% of it.
    const QImage src = boxDownscaleTo(equirect.convertToFormat(QImage::Format_RGBA8888), N * 4);
    const int W = src.width(), H = src.height();
    if (W <= 0 || H <= 0) return false;
    // Face basis in WORLD axes (+X,-X,+Y,-Y,+Z,-Z; dir = axis + right*u + up*v
    // with image row 0 at the top) — exactly what SkyDesc::reflectionFaces takes;
    // the engine converts to its cubemap handedness. The equirect fetch below
    // uses dirToEquirect, i.e. OGRE'S sky mapping, so a reflection lines up with
    // the sky pixel the camera sees in that direction.
    static const float ax[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    static const float rt[6][3] = {{0,0,-1},{0,0,1},{1,0,0},{1,0,0},{1,0,0},{-1,0,0}};
    static const float up[6][3] = {{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
    std::vector<unsigned char> face(size_t(N) * N * 4u);
    bool ok = true;
    // Bilinear fetch: wrap in u (the seam is continuous), clamp in v (the poles
    // are not). Kills the stair-stepping the old nearest fetch left on gradients.
    const auto fetch = [&](float ut, float vt, unsigned char *out) {
        float fx = ut * W - 0.5f, fy = vt * H - 0.5f;
        int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
        const float tx = fx - x0, ty = fy - y0;
        int x1 = x0 + 1, y1 = y0 + 1;
        x0 = ((x0 % W) + W) % W; x1 = ((x1 % W) + W) % W;
        y0 = std::min(H - 1, std::max(0, y0)); y1 = std::min(H - 1, std::max(0, y1));
        const unsigned char *r0 = src.constScanLine(y0), *r1 = src.constScanLine(y1);
        for (int c = 0; c < 4; ++c) {
            const float top = r0[size_t(x0) * 4u + c] * (1 - tx) + r0[size_t(x1) * 4u + c] * tx;
            const float bot = r1[size_t(x0) * 4u + c] * (1 - tx) + r1[size_t(x1) * 4u + c] * tx;
            out[c] = (unsigned char)std::lround(std::min(255.0f, std::max(0.0f, top * (1 - ty) + bot * ty)));
        }
    };
    for (int f = 0; f < 6 && ok; ++f) {
        const float *a = ax[f], *r = rt[f], *u = up[f];
        for (int py = 0; py < N; ++py) {
            const float uv = 1.0f - 2.0f * (py + 0.5f) / N;   // up multiplier, row 0 = top
            for (int px = 0; px < N; ++px) {
                const float ur = 2.0f * (px + 0.5f) / N - 1.0f;
                float dx = a[0] + r[0] * ur + u[0] * uv;
                float dy = a[1] + r[1] * ur + u[1] * uv;
                float dz = a[2] + r[2] * ur + u[2] * uv;
                const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= len; dy /= len; dz /= len;
                float ut, vt;
                dirToEquirect(dx, dy, dz, ut, vt);
                fetch(ut, vt, &face[(size_t(py) * N + px) * 4u]);
            }
        }
        ids[f] = mTarget->createTexture(unsigned(N), unsigned(N), face.data(), true);
        if (!ids[f]) ok = false;
    }
    return ok;
}

// THE PER-MATERIAL REFLECTION CUBEMAP (ADDENDUM A-5).
//
// v1 takes ONE image — an equirect panorama, the same shape the equirect sky
// takes — and projects it onto the six world-axis faces with the code above;
// Scene::createCubemap then copies them into a cube, applying the backend's
// left-handed remap once and in one place. The face textures are transient:
// the cube is a copy, so they are released the moment it exists.
//
// NOT a live texture (ADDENDUM A-1): a live texture's contract is that a
// generation bump re-uploads its pixels, and a cube built from it is a COPY
// that no updateTexture can reach — binding one here would silently freeze at
// its first frame. Refused by returning nothing rather than by lying.
TextureId SceneMirror::reflectionCubeFor(const QString &path)
{
    if (path.isEmpty() || !mTarget) return 0;
    if (iris::LiveTextures::isLiveRef(path)) return 0;
    // Its own key space: this entry is a CUBE, and handing it to a 2D slot
    // (or vice versa) would be a silently wrong bind rather than a miss.
    const QString key = QStringLiteral("cube|") + path;
    auto it = mTextures.constFind(key);
    if (it != mTextures.constEnd()) return it.value();
    if (path.startsWith(QLatin1Char(':')) || !QFileInfo::exists(path)) return 0;
    const QImage equirect = QImage(path);
    if (equirect.isNull()) return 0;

    TextureId faces[6] = { 0, 0, 0, 0, 0, 0 };
    TextureId cube = 0;
    if (buildEquirectCubeFaces(equirect, faces)) cube = mTarget->createCubemap(faces);
    for (int i = 0; i < 6; ++i) if (faces[i]) mTarget->destroyTexture(faces[i]);
    if (cube) mTextures.insert(key, cube);
    return cube;
}

// CPU port of irisgl/assets/shaders/realisticsky.frag (a Preetham-style analytic
// scattering shader, Three.js lineage). Faithful to the GLSL — including its
// quirks (the unused ExposureBias, the simplified Rayleigh term) — evaluated per
// equirect texel over the view direction; the sun's disc, colour and haze land
// exactly where the legacy renderer put them.
QImage SceneMirror::bakeRealisticSky(const iris::SkyRealistic &sky, int width, int height,
                                     bool forHdr)
{
    if (width <= 0 || height <= 0) return QImage();
    struct V3 {
        float x, y, z;
        V3(float v = 0) : x(v), y(v), z(v) {}
        V3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
        V3 operator+(const V3 &o) const { return V3(x + o.x, y + o.y, z + o.z); }
        V3 operator-(const V3 &o) const { return V3(x - o.x, y - o.y, z - o.z); }
        V3 operator*(const V3 &o) const { return V3(x * o.x, y * o.y, z * o.z); }
        V3 operator/(const V3 &o) const { return V3(x / o.x, y / o.y, z / o.z); }
        V3 operator*(float s) const { return V3(x * s, y * s, z * s); }
    };
    const auto vpow = [](const V3 &v, float e) {
        return V3(std::pow(std::max(0.0f, v.x), e), std::pow(std::max(0.0f, v.y), e),
                  std::pow(std::max(0.0f, v.z), e));
    };
    const auto vexp = [](const V3 &v) { return V3(std::exp(v.x), std::exp(v.y), std::exp(v.z)); };
    const float pi = 3.14159265358979f;
    // Filmic tonemap constants (Uncharted2), verbatim from the shader.
    const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f, W = 1000.0f;
    const auto tonemap = [&](const V3 &v) {
        const auto f1 = [&](float x) {
            return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
        };
        return V3(f1(v.x), f1(v.y), f1(v.z));
    };

    // Per-image terms (uniform across directions).
    const float luminance = std::max(0.01f, sky.luminance);
    const float sunfade = 1.0f - std::min(1.0f, std::max(0.0f, 1.0f - std::exp(sky.sunPosY / 450000.0f)));
    const float reileighCoefficient = sky.reileigh - (1.0f * (1.0f - sunfade));
    V3 sunDirection(sky.sunPosX, sky.sunPosY, sky.sunPosZ);
    {
        const float len = std::sqrt(sunDirection.x * sunDirection.x + sunDirection.y * sunDirection.y +
                                    sunDirection.z * sunDirection.z);
        if (len > 1e-6f) sunDirection = sunDirection * (1.0f / len); else sunDirection = V3(0, 1, 0);
    }
    const float cutoffAngle = pi / 1.95f, steepness = 1.5f, EE = 1000.0f;
    const float sunE = EE * std::max(0.0f, 1.0f - std::exp(-((cutoffAngle - std::acos(std::min(1.0f, std::max(-1.0f, sunDirection.y)))) / steepness)));
    const V3 betaR = V3(0.0005f / 94.0f, 0.0005f / 40.0f, 0.0005f / 18.0f) * reileighCoefficient;
    // totalMie(lambda, K, T) * mieCoefficient; lambda/K/v verbatim.
    const V3 lambda(680e-9f, 550e-9f, 450e-9f);
    const V3 K(0.686f, 0.678f, 0.666f);
    const float mieC = (0.2f * sky.turbidity) * 1e-17f;   // (0.2*T)*10E-18 in GLSL
    const V3 betaM = V3(0.434f * mieC * pi * std::pow(2.0f * pi / lambda.x, 2.0f) * K.x,
                        0.434f * mieC * pi * std::pow(2.0f * pi / lambda.y, 2.0f) * K.y,
                        0.434f * mieC * pi * std::pow(2.0f * pi / lambda.z, 2.0f) * K.z) * sky.mieCoefficient;
    const V3 betaRM = betaR + betaM;
    const V3 whiteScale = V3(1, 1, 1) / tonemap(V3(W));
    const float sunAngularDiameterCos = 0.99995667694644844f;
    const float horizonMix = std::min(1.0f, std::max(0.0f, std::pow(std::max(0.0f, 1.0f - sunDirection.y), 5.0f)));
    const float exposure = std::log2(2.0f / std::pow(luminance, 4.0f));
    const float finalGamma = 1.0f / (1.2f + (1.2f * sunfade));

    QImage img(width, height, QImage::Format_RGBA8888);
    for (int row = 0; row < height; ++row) {
        unsigned char *out = img.scanLine(row);
        const float v = (row + 0.5f) / height;
        for (int col = 0; col < width; ++col) {
            // equirectDir: the mapping Ogre's sky shader reads the bake back
            // with, so the sun lands in the world direction sunPos names.
            float dx, dy, dz;
            equirectDir((col + 0.5f) / width, v, dx, dy, dz);
            const V3 dir(dx, dy, dz);

            const float zenithAngle = std::acos(std::max(0.0f, dir.y));
            const float denom = std::cos(zenithAngle) +
                                0.15f * std::pow(93.885f - zenithAngle * 180.0f / pi, -1.253f);
            const float sR = 8.4e3f / denom, sM = 1.25e3f / denom;
            const V3 Fex = vexp(V3(-(betaR.x * sR + betaM.x * sM), -(betaR.y * sR + betaM.y * sM),
                                   -(betaR.z * sR + betaM.z * sM)));

            const float cosTheta = dir.x * sunDirection.x + dir.y * sunDirection.y + dir.z * sunDirection.z;
            const float rp = cosTheta * 0.5f + 0.5f;
            const float rPhase = (3.0f / (16.0f * pi)) * (1.0f + rp * rp);
            const float g = sky.mieDirectionalG;
            const float mPhase = (1.0f / (4.0f * pi)) *
                ((1.0f - g * g) / std::pow(std::max(1e-6f, 1.0f - 2.0f * g * cosTheta + g * g), 1.5f));
            const V3 betaTheta = betaR * rPhase + betaM * mPhase;
            const V3 ratio = betaTheta / betaRM;

            V3 Lin = vpow(ratio * sunE * (V3(1, 1, 1) - Fex), 1.5f);
            const V3 linB = vpow(ratio * sunE * Fex, 0.5f);
            Lin = Lin * (V3(1.0f - horizonMix) + linB * horizonMix);

            // Night-sky base + the solar disc.
            V3 L0 = Fex * 0.1f;
            const float sundisk = cosTheta <= sunAngularDiameterCos ? 0.0f
                : cosTheta >= sunAngularDiameterCos + 0.00002f ? 1.0f
                : [&] { const float t = (cosTheta - sunAngularDiameterCos) / 0.00002f; return t * t * (3.0f - 2.0f * t); }();
            L0 = L0 + Fex * (sunE * 19000.0f * sundisk);

            V3 texColor = (Lin + L0) * 0.04f + V3(0.0f, 0.001f, 0.0025f) * 0.3f;
            // Two gradings are one too many (POST_CHAIN_SPEC §7.1). Without the
            // post chain the bake IS the grade — Uncharted2 plus a gamma, and
            // the result goes to an LDR viewport. With the chain on, the chain's
            // own filmic tonemapper grades everything else in the frame, so the
            // sky must arrive UNgraded or it is rolled off twice and reads flat.
            // The exposure term stays either way: it is what makes a luminance
            // setting mean anything.
            V3 colr = texColor * exposure;
            if (!forHdr) {
                colr = tonemap(colr) * whiteScale;
                colr = vpow(colr, finalGamma);
            } else {
                // Still 8-bit storage, so the top end has to land somewhere:
                // Reinhard is the gentlest possible mapping into [0,1] and, unlike
                // Hable + gamma, leaves the midtones where the chain expects them.
                colr = V3(colr.x / (1.0f + colr.x), colr.y / (1.0f + colr.y),
                          colr.z / (1.0f + colr.z));
            }

            const auto to8 = [](float v) {
                if (!std::isfinite(v)) v = 0.0f;
                return (unsigned char)std::lround(std::min(1.0f, std::max(0.0f, v)) * 255.0f);
            };
            unsigned char *p = out + size_t(col) * 4u;
            p[0] = to8(colr.x); p[1] = to8(colr.y); p[2] = to8(colr.z); p[3] = 255;
        }
    }
    return img;
}

/// Translates a document camera into the engine's CameraDesc — the shared half
/// of applyCamera and applyPip (CAMERAS_SPEC §7.7). It reads the node and
/// nothing else, so both the main view and the inset see the same lens.
static CameraDesc toCameraDesc(const iris::CameraNodePtr &camera)
{
    CameraDesc c;
    camera->update(0.0f);
    c.position     = toVec3(camera->getGlobalPosition());
    c.orientation  = toQuat(camera->getGlobalRotation());
    c.fovDegrees   = camera->angle > 0.0f ? camera->angle : 45.0f;
    c.nearClip     = camera->nearClip;
    c.farClip      = camera->farClip;
    c.orthographic = !camera->isPerspective;
    c.orthoSize    = camera->orthoSize;
    // Phase 2c: the letterbox travels with the lens, so any view showing this
    // camera constrains it the same way (§7.4).
    c.constrainAspect = camera->constrainAspect;
    c.aspect          = camera->aspectRatio > 0.01f ? camera->aspectRatio : 16.0f / 9.0f;
    // CAMERA_LENS_SPEC §3: the shift travels as a FRACTION of the frame and the
    // engine converts it, because the conversion needs the aspect the VIEW is
    // rendering at (Types.h says why at length). Zero is the default and is a
    // symmetric frustum, bit for bit.
    c.lensShiftX      = camera->lensShiftX;
    c.lensShiftY      = camera->lensShiftY;
    return c;
}

// ---------------------------------------------------------------------------
// THE PER-CAMERA LOOK (CAMERA_LENS_SPEC §4/§5).
//
// A scene camera may carry its own EXPOSURE (a mode plus stops) and its own
// POST OVERRIDES (a tri-state map over the world's values). `applyCameraPostFx`
// below is the ONE function that resolves either of them, over whatever
// description the world produced for the view being drawn.
//
// IT IS CALLED FROM TWO PLACES, and the pair is deliberate:
//   * applyEnvironment, where the world's PostFxDesc is built — so the value
//     the engine receives is already final and STABLE frame to frame. That is
//     not tidiness: an enable-flag difference is a workspace rebuild, and a
//     world description followed a moment later by a camera description that
//     disagreed with it would rebuild the compositor twice every frame;
//   * applyCamera, right after the active-camera seam has decided which camera
//     is driving — so a CUT, a first frame, or a one-shot screenshot view that
//     applyEnvironment had no record for still grades correctly, immediately.
// In the steady state the second push is the same value as the first and the
// engine drops it for free.
//
// RESOLUTION IS PER VIEW, which is why the driving camera is remembered per
// view (noteDrivingCamera): the editor viewport, the player and a screenshot's
// throwaway view are served by ONE mirror, and a mirror-level "the driving
// camera" would hand the last camera applied anywhere to all of them.
//
// IT COSTS NOTHING when the camera says nothing: cameraOverridesAnything is
// false and the world's description is passed through untouched, bit for bit —
// which is what the byte-identical offscreen negative in tests/cameras asserts.
//
// THE DETERMINISM LAW is not re-implemented here and must not be: an offscreen
// view discards the whole post description inside the engine unless a caller
// deliberately opted in (PostFxDesc::allowOffscreen, Types.h). Thumbnails,
// previews and every pixel suite are therefore untouchable by construction, and
// `screenshot({camera, postFx:true})` gets the camera's grade through the same
// substitution because it opts in AFTER this runs.
bool SceneMirror::noteDrivingCamera(const View *view, const iris::CameraNodePtr &camera)
{
    // A tiny bounded LRU rather than a map: screenshot views are created and
    // destroyed per call, so an unbounded per-view table would grow forever
    // with keys that can never be looked up again. Eight is more views than any
    // host has ever had at once, and a stale View key is only ever COMPARED —
    // nothing here dereferences one.
    //
    // The CAMERA is held WEAKLY, and that is not caution for its own sake: the
    // entry is read back a frame later by applyEnvironment, which needs the
    // node's fields. A camera deleted between the two calls would otherwise be
    // a read-after-free with a one-frame window — small, real, and exactly the
    // kind of thing that turns up once a month in a crash log.
    constexpr int kMaxTracked = 8;
    for (auto &entry : mDrivingCameras) {
        if (entry.first != view) continue;
        // QWeakPointer has no data() — lock and compare, which also makes a
        // camera that DIED since the last frame read as a change (it is one).
        const bool changed = entry.second.toStrongRef().data() != camera.data();
        entry.second = camera.toWeakRef();
        return changed;
    }
    if (int(mDrivingCameras.size()) >= kMaxTracked) mDrivingCameras.erase(mDrivingCameras.begin());
    mDrivingCameras.emplace_back(view, camera.toWeakRef());
    // The FIRST camera a view is ever given is not a cut: there is no previous
    // shot to cut from, and re-seeding the exposure history on the first frame
    // would change the opening frames of every view that ever existed.
    return false;
}

iris::CameraNodePtr SceneMirror::drivingCameraFor(const View *view) const
{
    for (const auto &entry : mDrivingCameras)
        if (entry.first == view) return entry.second.toStrongRef();
    return iris::CameraNodePtr();
}

static bool cameraOverridesAnything(const iris::CameraNodePtr &camera)
{
    if (!camera) return false;
    return camera->exposureMode != iris::CameraExposureMode::Inherit ||
           !camera->postOverrides.isEmpty();
}

/// Applies the camera's exposure block and override map over `fx`.
static void applyCameraPostFx(const iris::CameraNodePtr &camera, PostFxDesc &fx)
{
    // ---- §4, exposure. STOPS in the document, the chain's natural-log axis
    // here, converted in ONE place (iris::lens::exposureStopsToChain).
    if (camera->exposureMode != iris::CameraExposureMode::Inherit) {
        fx.exposure = iris::lens::exposureStopsToChain(camera->exposure);
        if (camera->exposureMode == iris::CameraExposureMode::Manual) {
            // min == max pins the shader's clamp, which is what makes the grade
            // a NUMBER instead of a measurement. The pin is a constant and not
            // the exposure — see iris::lens::manualExposureClamp for why using
            // the exposure would make one authored stop move the picture by two.
            fx.exposureMin = fx.exposureMax = iris::lens::manualExposureClamp();
        } else {
            fx.exposureMin = iris::lens::exposureStopsToChain(camera->exposureMin);
            fx.exposureMax = iris::lens::exposureStopsToChain(camera->exposureMax);
            if (fx.exposureMax < fx.exposureMin) std::swap(fx.exposureMin, fx.exposureMax);
        }
    }

    // ---- §5, the tri-state overrides. Absent = inherit, so every branch below
    // is guarded by hasPostOverride and the world's value survives otherwise.
    const auto num = [&camera](const char *id, float &field) {
        const QVariant v = camera->postOverride(QLatin1String(id));
        if (v.isValid()) field = v.toFloat();
    };
    const auto flag = [&camera](const char *id, bool &field) {
        const QVariant v = camera->postOverride(QLatin1String(id));
        if (v.isValid()) field = v.toInt() != 0;
    };
    const auto whole = [&camera](const char *id, int &field) {
        const QVariant v = camera->postOverride(QLatin1String(id));
        if (v.isValid()) field = v.toInt();
    };
    flag("hdr", fx.hdr);
    flag("bloom", fx.bloom);
    num("bloomThreshold", fx.bloomThreshold);
    num("bloomKnee", fx.bloomKnee);
    flag("ssao", fx.ssao);
    num("ssaoPower", fx.ssaoPower);
    num("ssaoRadius", fx.ssaoRadius);
    // SMAA accepts only "off" from a camera (the document refuses anything
    // else): a per-camera PRESET would be a shader recompile on every cut.
    whole("smaa", fx.smaaPreset);
    whole("ssr", fx.ssr);
    // Refraction is the world's three-state mode (0 off / 1 auto / 2 always) on
    // the document side but a BOOL by the time it reaches the view — the "auto"
    // resolution against mAnyRefractive has already happened. So a camera
    // override of 1 (auto) must not be read as "on": it means "whatever the
    // world just resolved", which is precisely what leaving fx alone does.
    {
        const QVariant v = camera->postOverride(QLatin1String("refractions"));
        if (v.isValid() && v.toInt() != 1) fx.refractions = v.toInt() == 2;
    }
    // Distortion is the world's three-state mode resolved to a BOOL by the time
    // it reaches the view — exactly like refractions above, including the rule
    // that an override of 1 (auto) means "whatever the world just resolved".
    {
        const QVariant v = camera->postOverride(QLatin1String("distortion"));
        if (v.isValid() && v.toInt() != 1) fx.distortion = v.toInt() == 2;
    }
    num("distortionStrength", fx.distortionStrength);

    // THE LOOKS STACK, whole (POST_LOOKS_SPEC §4.1 / D4). Present REPLACES the
    // world's stack — including with an empty one, which is how a camera says
    // "no looks" over a world that has them; absent inherits, so the world's
    // resolved stack survives untouched, which is what keeps the byte-identical
    // negative in tests/cameras true for every camera that says nothing.
    if (camera->hasPostOverride(QStringLiteral("looks")))
        fx.looks = resolveLooks(camera->postOverrideStack(QStringLiteral("looks")));
}

void SceneMirror::applyPip(iris::CameraNodePtr camera, View *view, const ViewPipDesc &desc)
{
    if (!view) return;
    ViewPipDesc d = desc;
    if (!camera || !d.enabled) {
        // OFF is a value, not a special case: the engine tears the second
        // workspace down and the frame is byte-identical to one that never had
        // an inset. Keep the rest of the desc so a host that toggles does not
        // also have to re-state its rect.
        d.enabled = false;
        view->setPip(d);
        return;
    }
    d.camera = toCameraDesc(camera);

    // THE INSET'S GRADE (CAMERAS_SPEC §7.2 Route C / POST_CHAIN_SPEC §14).
    //
    // The same substitution the lens program built, at the same one function,
    // over the WORLD's description rather than the view's: the inset renders
    // the PIPPED camera, so its exposure and its post overrides are the ones
    // that decide how the inset looks — and they decide it for the inset ONLY,
    // because they never reach view->setPostFx. That is the whole per-camera
    // preview: a camera at -2 stops darkens its own inset and leaves the main
    // viewport exactly where it was.
    //
    // What survives the trip is what a secondary surface can honour: WHETHER
    // the shot is tonemapped (fx.hdr — so the inset grades exactly when the
    // main view's chain does, and a camera that overrides `hdr` decides it for
    // its own preview) and the EXPOSURE it is tonemapped at. Bloom, AO, SMAA
    // and SSR are not a second post chain's worth of machinery for a 200-pixel
    // rectangle, and §14 makes the same call for thumbnails: a preview is a
    // photograph of the content, not of the scene's quality tier.
    {
        PostFxDesc fx = mWorldPostFx;
        applyCameraPostFx(camera, fx);
        d.tonemap  = fx.hdr;
        d.exposure = fx.exposure;
    }
    view->setPip(d);
}

void SceneMirror::applyCamera(iris::CameraNodePtr camera, View *view, float framingAspect)
{
    if (!camera || !view) return;

    // The camera the HOST handed over, before any substitution below. The
    // wide-aspect framing hold describes THAT camera ("mine, a free explorer"),
    // so if the active-camera seam swaps in an AUTHORED camera the hold must
    // not follow: an authored lens is a deliberate choice and stays exactly as
    // authored, at every aspect (owner rule, 2026-09-07).
    const iris::CameraNode *hostCamera = camera.data();

    // THE ACTIVE-CAMERA SEAM (CAMERAS_SPEC D6, phase 1). This function is the
    // ONLY way a View's camera moves (Engine.h), so the whole of "play renders
    // through the designated scene camera" is one substitution of the SOURCE
    // node — every caller keeps passing whatever camera it owns.
    //
    // Two conditions, both required:
    //   * the document says it is PLAYING (PlayBack sets it, for the editor's
    //     play-in-place and for the player view alike). EDITING must never
    //     route through the active camera — the main viewport stays the
    //     explorer until phase 3's pilot mode.
    //   * an active camera is set and still resolves.
    // Preview scenes (thumbnails, material/asset/avatar previews) have their
    // own documents, which are never playing and have no active camera, so they
    // are untouched by construction.
    //
    // ...with ONE exception, and it is a mode choice rather than a special case
    // (AVATAR_LOCOMOTION_SPEC §8.5): while an avatar is POSSESSED, the
    // spring-arm follow camera is what the player is looking through, and the
    // arm drives `Scene::camera` — the very camera the host already passed in.
    // A scene that has both an armed active camera and a possessed character
    // has said which one it wants by possessing; letting the active camera win
    // there would render the shot from a tripod while the user drove a
    // character they could not see.
    if (mSource && mSource->isPlaying()) {
        const iris::AvatarPossession *possession = mSource->getPossession();
        const bool possessing = possession && possession->isPossessing();
        if (!possessing)
            if (auto active = mSource->getActiveCamera()) camera = active;
    }

    // A CAMERA NEVER DRAWS ITSELF (CAMERAS_SPEC phase 2b). Whatever camera is
    // driving the view is, by definition, the one whose body would sit on the
    // near plane and whose frustum lines would fan across the whole image —
    // while piloting it (phase 3), while playing through the active camera, or
    // in any preview whose viewpoint happens to be a scene node. Recorded here
    // because this function is "the ONLY way a View's camera moves" (Engine.h),
    // so it is the one place that always knows.
    //
    // IS THIS A CUT, FOR THIS VIEW? Recorded first, and deliberately NOT as
    // `mViewCamera != camera`: that field is mirror-wide, and one mirror serves
    // several views (the viewport, a screenshot's throwaway view, the
    // player's). Two views showing two different cameras would flip it every
    // frame and read as a cut every frame — which would pin the auto exposure
    // to its seed forever. The answer has to be per view, so it is remembered
    // per view, and applyEnvironment reads the same record next frame.
    const bool cut = noteDrivingCamera(view, camera);

    if (mViewCamera != camera.data()) {
        mViewCamera = camera.data();
        // ...and hide its helpers NOW rather than at the next sync. Hosts call
        // sync() and applyCamera() in either order (the editor syncs first, the
        // gizmo suite applies first), and a viewport that renders one frame
        // between them would show the camera's own frustum fanning across the
        // whole image. Cheap: one hash lookup on a change only.
        auto it = mEntries.find(mViewCamera);
        if (it != mEntries.end() && it->wireNode) mTarget->setNodeVisible(it->wireNode, false);
    }

    // THE CAMERA'S OWN LOOK (CAMERA_LENS_SPEC §4/§5) — the second of the two
    // call sites the model documents; in the steady state applyEnvironment has
    // already pushed exactly this value and the engine drops the repeat. What
    // this one is FOR is the frames applyEnvironment could not get right: the
    // cut, the first frame, and a one-shot screenshot view it had no record of.
    // Nothing happens, and nothing is pushed, for a camera that overrides
    // nothing.
    if (cameraOverridesAnything(camera)) {
        PostFxDesc fx = view->postFx();   // the world's, as applyEnvironment left it
        applyCameraPostFx(camera, fx);
        view->setPostFx(fx);
        // A CUT IS NOT A LIGHTING CHANGE. The chain's auto exposure adapts at
        // ~75%/s, so without this a cut to a differently exposed camera fades
        // over one to two seconds — including a MANUAL one, whose clamp pins
        // what is measured but not what the history holds. Re-seeded AFTER the
        // new description is pushed, because the seed is derived from it.
        if (cut) view->resetExposureHistory();
    } else if (cut) {
        // Cutting AWAY from an overriding camera to one that inherits: the
        // world's description is already back on the view (applyEnvironment
        // pushes it every frame), but the history still holds the old camera's
        // grade. Same re-seed, same reason.
        view->resetExposureHistory();
    }

    CameraDesc desc = toCameraDesc(camera);
    // The WIDE-ASPECT FRAMING HOLD is the HOST's statement about the camera it
    // just handed over ("this one is a free explorer"), not a property of the
    // document node — a camera the user authored keeps its lens whoever renders
    // it — so it is applied HERE and not inside toCameraDesc, which applyPip
    // also uses and which must never hold an authored camera's inset.
    desc.framingAspect = (camera.data() == hostCamera) ? framingAspect : 0.0f;
    view->setCamera(desc);

    // A CAMERA ON A SOCKET RIDES ITS NODE (AVATAR_RIG_PERF_SPEC §4.6, P2b).
    //
    // The description above carries the camera's world transform as it was
    // BEFORE this frame — which for a socketed camera is a bone pose from the
    // last frame, because tag points resolve inside the frame. The rider NODE is
    // exact; the desc is not. Attaching the engine's camera to that node closes
    // the last frame of lag for a first-person avatar, and does nothing at all
    // for every other camera: the binding is armed ONLY while the camera is
    // socketed, and dropped the moment it is not (so a camera the user takes off
    // a head goes straight back to the pushed pose).
    jahshaka::engine::NodeId ride = 0;
    if (!camera->socketOwnerGuid.isEmpty()) {
        const auto it = mEntries.constFind(camera.data());
        if (it != mEntries.constEnd()) ride = it->node;
    }
    if (view->cameraNode() != ride) view->setCameraNode(ride);
}
