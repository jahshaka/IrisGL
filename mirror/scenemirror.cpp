#include "core/math/mat3.h"
#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "core/color.h"
#include "irisgl/mirror/scenemirror.h"

#include <cstring>
#include <algorithm>
#include <cstdlib>
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
#include <chrono>

namespace {

/// ONE MIRROR SUB-STAGE, folded into the frame record the engine is building
/// (RENDER_LOOP_MONITOR_SPEC §4.2). Constructed with a null engine — which is
/// every frame with no capture running — it reads no clock and does nothing.
/// The scopes below are SEQUENTIAL, never nested, so the times they report are
/// exclusive by construction and sum to the mirror's own stage.
struct MirrorStage {
    jahshaka::engine::Engine *engine;
    const char *name;
    std::chrono::steady_clock::time_point start;
    MirrorStage(jahshaka::engine::Engine *e, const char *n) : engine(e), name(n)
    {
        if (engine) start = std::chrono::steady_clock::now();
    }
    ~MirrorStage()
    {
        if (!engine) return;
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start).count();
        engine->noteHostStage(std::string(name), float(ms));
    }
    MirrorStage(const MirrorStage &) = delete;
    MirrorStage &operator=(const MirrorStage &) = delete;
};

}   // namespace
#include <QtMath>

using namespace jahshaka::engine;

namespace {
inline Vec3 toVec3(const iris::Vec3 &v) { return Vec3(v.x(), v.y(), v.z()); }
inline Quat toQuat(const iris::Quat &q) { return Quat(q.x(), q.y(), q.z(), q.scalar()); }

/// A SUN THAT HAS SET (SUN_FOLLOWS_ATMOSPHERE, round-2 review item 4). At and
/// below the horizon the atmosphere's tint runs to nothing — measured on the
/// shipped preset, 6e-4 / 2.5e-6 / 1.4e-10 at elevation zero — so the light
/// contributes no pixel anywhere. Its SHADOW is not free, though: a directional
/// caster renders three full-view-frustum PSSM passes every frame it is on, and
/// the sun disc would go on being drawn in a night sky. Both are dropped once
/// the brightest channel falls below this, which is a thousandth of the noon
/// value and three hundred times below one 8-bit step.
constexpr float kSunNightTint = 1e-3f;
inline bool sunTintIsNight(const jahshaka::engine::Colour &t)
{
    return std::max(std::max(t.r, t.g), t.b) < kSunNightTint;
}


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

/// THE LIGHT'S GI PARAMETERS as a change key (ENGINE_CACHE_POLICY_SPEC P7).
/// Folded into the GI light signature beside the light's transform: VCT
/// injects each light's colour x intensity over its reach and cone, so a
/// colour, intensity, range, type, spot or area-shape edit stales the voxel
/// solve exactly as moving the light does. Before this the signature hashed
/// TRS only, and such an edit never reached the bounce at all (nor, once the
/// endless probe sweep went, the reflections). The same debounce then applies:
/// a slider drag re-injects on the cheap cadence and re-solves once on release.
///
/// And the light's EFFECTIVE shown state (its own flag and every ancestor's):
/// VctLighting injects only visible lights, so switching a lamp off is as much
/// a GI input as dimming it to zero (code review 2026-09-12).
quint64 lightGiParamSignature(const iris::LightNode *l)
{
    Hasher h;
    h << int(l->lightType) << l->color.rgba() << l->intensity << l->iesNormalisation
      << l->distance << l->spotCutOff << l->spotCutOffSoftness << l->spotFalloff
      << l->rectWidth << l->rectHeight << l->doubleSided << l->accurate
      << l->isVisibleInScene();
    return h.h;
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
    // THE VERIFICATION SWITCH (DIRTY_SET_MIRROR_SPEC §3.8 item 2). `full` runs
    // the whole walk EVERY sync — ruinous for frame time and exact by
    // construction, which is what the differential suite runs under. A number
    // sets the amortised verifier's per-sync budget (0 turns it off).
    mTrace = std::getenv("JAH_MIRROR_TRACE") != nullptr;
    if (const char *v = std::getenv("JAH_MIRROR_VERIFY")) {
        const QByteArray mode(v);
        if (mode == "full") mVerifyEverything = true;
        else if (!mode.isEmpty()) {
            bool ok = false;
            const int k = mode.toInt(&ok);
            if (ok && k >= 0) mVerifierBudget = unsigned(k);
        }
    }
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
    // A BIND IS A FULL-WALK TRIGGER (DIRTY_SET_MIRROR_SPEC §3.5): every entry
    // below is released, so there is nothing incremental left to be right
    // about. The aggregates the entries carried go with them.
    mFullWalkPending = true;
    mMaterialUsers.clear();
    mDirtyScratch.clear();
    mEvictedScratch.clear();
    mMovableNodes = 0;
    mMovableLights.clear();
    mSkinnedNodes = 0;
    mCharacterPieces = 0;
    mRefractiveEntries = 0;
    mDistortionEntries = 0;
    mAnyRefractive = false;
    mAnyDistortion = false;
    mVerifierCursor = 0;
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
    // The ground's horizon: same discipline as the grid. Its MATERIAL is the
    // floor's own and belongs to mMaterials, which is swept below — dropping it
    // here would destroy the floor's material out from under the floor.
    if (mHorizonNode) { mTarget->removeNode(mHorizonNode); mHorizonNode = 0; }
    if (mHorizonMesh) { mTarget->destroyMesh(mHorizonMesh); mHorizonMesh = 0; }
    mHorizonMeshSource = nullptr;
    mHorizonMaterial = 0;
    mHorizonVisible = -1;
    mHorizonFloor = nullptr;
    // The GI volume overlay's two boxes (fix 9): same discipline as the grid.
    if (mGiVolLitNode)   { mTarget->removeNode(mGiVolLitNode);   mGiVolLitNode = 0; }
    if (mGiVolProbeNode) { mTarget->removeNode(mGiVolProbeNode); mGiVolProbeNode = 0; }
    if (mGiVolLitMesh)   { mTarget->destroyMesh(mGiVolLitMesh);   mGiVolLitMesh = 0; }
    if (mGiVolProbeMesh) { mTarget->destroyMesh(mGiVolProbeMesh); mGiVolProbeMesh = 0; }
    if (mGiVolLitMaterial)   { mTarget->destroyMaterial(mGiVolLitMaterial);   mGiVolLitMaterial = 0; }
    if (mGiVolProbeMaterial) { mTarget->destroyMaterial(mGiVolProbeMaterial); mGiVolProbeMaterial = 0; }
    mGiVolBuilt = false;
    mHighlighted.clear();
    mHighlightSet.clear();
    mHighlightRequestedPrimary.clear();
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
    mMaterialSync.clear();     // the per-material memo dies with the materials
    mMaterialItemSerial.clear();
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
    mSkySource = SkySource();
    mSkyDesc = SkyDesc();
    for (float &c : mSkyAmbientSh) c = 0.0f;
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
    // Same as a bind: the entries go, so the mirror knows nothing about the
    // document any more and the next sync has to look at all of it.
    mFullWalkPending = true;
    mMaterialUsers.clear();
    mDirtyScratch.clear();
    mEvictedScratch.clear();
    mMovableNodes = 0;
    mMovableLights.clear();
    mSkinnedNodes = 0;
    mCharacterPieces = 0;
    mRefractiveEntries = 0;
    mDistortionEntries = 0;
    mAnyRefractive = false;
    mAnyDistortion = false;
    mVerifierCursor = 0;
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
    mHighlightSet.clear();
    mHighlightRequestedPrimary.clear();
    // THE GROUND'S HORIZON GOES TOO (lead review). Its `mHorizonFloor` is a raw
    // pointer into the document that is leaving; its material PIN is what keeps
    // the floor's datablock out of the sweep, and every entry that referenced
    // that datablock has just been released — so a horizon left behind holds a
    // material alive, on an Item, in a scene nobody renders, until the page
    // switches back. Same three lines setSource has.
    if (mHorizonNode) { mTarget->removeNode(mHorizonNode); mHorizonNode = 0; }
    if (mHorizonMesh) { mTarget->destroyMesh(mHorizonMesh); mHorizonMesh = 0; }
    mHorizonMeshSource = nullptr;
    mHorizonMaterial = 0;
    mHorizonVisible = -1;
    mHorizonFloor = nullptr;
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
quint64 SceneMirror::staticNodeCount() const
{
    return quint64(iris::graph::staticNodeCount());
}

void SceneMirror::onMaterialItemsRebuilt(MaterialId material)
{
    if (!material) return;
    // O(1), NOT O(every entry in the scene) (MIRROR_SCALE lane, 2026-09-13).
    //
    // A shading-model switch rebuilds every Item the material draws, and each
    // new Item is born with the default query mask — so every entry using it
    // owes one re-push of its `pickable` flag. That was done by walking the
    // whole entry hash, and the walk ran from visit() the FIRST time each
    // material was seen (shadingModelPushed starts at -1, and an attach resets
    // it): on a scene open with N nodes each carrying its own material — which
    // is exactly what the editor creates, one PbrMaterial per primitive — that
    // is N walks of N entries. Measured on a 2020-node lattice: 84 ms of
    // adopting sync against 40 ms for the same nodes sharing one material, and
    // the gap is quadratic, so an 8000-node scene paid for it sixteenfold.
    //
    // Now the material carries a SERIAL and the entry remembers which one it
    // pushed against. Same statement, no walk: the re-push happens on the
    // entry's own next visit, which is the frame it would have happened on
    // anyway (the walk only moved a latch).
    ++mMaterialItemSerial[material];
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
        // A RE-TAKE IS A FULL-WALK TRIGGER (§3.5): the other mirror's
        // evacuation hook released every entry, and the migration rebuilt every
        // Ogre node under this manager.
        mFullWalkPending = true;
    }
    // NO transform refresh. There is nothing to refresh: the document's world
    // transforms ARE Ogre's, resolved by the engine's threaded SIMD pass inside
    // the frame and, for the readers that need one between frames, on demand
    // (iris::graph::globalTransform -> Node::_getFullTransformUpdated).
    // Sockets (CAMERAS_SPEC §5) move nodes, so they resolve BEFORE the walk
    // that pushes transforms — a camera on a character's head has to be on the
    // head in the frame that renders it, not in the one after.
    // THE CAPTURE GATE (RENDER_LOOP_MONITOR_SPEC §4.2): ONE virtual call per
    // sync when nothing is capturing, and every scope below is then a pair of
    // no-ops on a null pointer.
    jahshaka::engine::Engine *mon =
        (mMonitorEngine && mMonitorEngine->frameMonitor() != jahshaka::engine::MonitorLevel::Off)
            ? mMonitorEngine : nullptr;
    { MirrorStage s(mon, "mirror.sockets");
    resolveSockets();
    }

    ++mSyncStamp;
    mVisited = 0;
    mMaterialBuilds = 0;    // per-walk, not a running total (materialBuildCount)
    mDirtyNodes = 0;
    mEvictedNodes = 0;
    mVerifierVisits = 0;
    // (mCharacterPieces / mSkinnedNodes are NOT reset here any more: they are
    // maintained on each entry's own transition — noteRigCounts — because a
    // still frame runs no walk to recount them. §3.7.)
    // SCENE_STATIC RE-PROMOTION, ON SETTLE (MIRROR_SCALE lane, 2026-09-13).
    //
    // Rule 4 (nodegraph.h) DEMOTES a static subtree on the first transform
    // write, and that is right: re-running the static pass per frame of a drag
    // is exactly the cost SCENE_STATIC exists to avoid. But nothing ever put
    // the node back — the demotion lasted the SESSION, so every prop a user
    // nudged spent the rest of the day in Ogre's per-frame transform and bounds
    // passes, and a long editing session drained the classification to nothing.
    //
    // The missing half is here: when NOTHING in the document has written a
    // transform for kStaticSettleFrames consecutive syncs, the document's own
    // static pass runs once and re-derives the whole scene's classification.
    // It is the same pass a load runs (SceneNode::applyStaticDefaults), so a
    // settled scene ends up classified exactly as if it had just been opened —
    // the user's own Static/Movable settings included, because the pass honours
    // overrides and writes none. A node whose class does not change costs the
    // walk and nothing else; one that does gets its notifyStaticDirty from
    // `switchOne`, one per promoted node, on this frame alone.
    //
    // THE GATE IS THE DOCUMENT'S TRANSFORM-WRITE COUNTER, which is global and
    // therefore conservative in the direction that cannot hurt: a physics step,
    // a playing animation, a drag anywhere in the scene holds the whole scene
    // dynamic until it stops. That is what "settle" has to mean — a promotion
    // in the middle of a gesture would migrate a subtree the next write
    // migrates straight back.
    if (mSource) {
        const unsigned long long writes = iris::graph::transformWrites();
        // ...AND ONLY WHEN SOMETHING WAS DEMOTED (lead review F4). The write
        // counter alone re-armed the settle on every transform write anywhere —
        // the end of a camera orbit, an undo, a reparent, a scene open — and
        // each quiet spell after one of those bought a whole-tree
        // applyStaticDefaults (a resolveMobility and an isStaticEligible per
        // node) to re-derive a classification nothing had disturbed. The
        // document counts its own demotions now, and that is the only thing a
        // re-promotion has to answer.
        const unsigned long long demotions = iris::graph::staticDemotions();
        if (writes != mLastTransformWrites) {
            mLastTransformWrites = writes;
            mSettleFrames = 0;
            if (demotions != mLastStaticDemotions) {
                mLastStaticDemotions = demotions;
                mStaticSettlePending = true;
            }
        } else if (mStaticSettlePending && ++mSettleFrames >= kStaticSettleFrames) {
            if (auto root = mSource->getRootNode()) {
                root->applyStaticDefaults();
                ++mStaticRepromotions;
            }
            mStaticSettlePending = false;
            // applyStaticDefaults migrates nodes between memory managers; the
            // migration itself must not read as a write, or a demotion, and
            // re-arm the settle.
            mLastTransformWrites = iris::graph::transformWrites();
            mLastStaticDemotions = iris::graph::staticDemotions();
        }
    }
    // The focus-smoothing dt for this walk (CAMERA_LENS_SPEC §3 P2). Zero on
    // the first sync, and capped at a tenth of a second: a stall must not let a
    // tracking camera jump its whole remaining focus travel in one frame.
    if (!mFocusClock.isValid()) { mFocusClock.start(); mFocusDt = 0.0f; }
    else mFocusDt = std::min(0.1f, float(mFocusClock.restart()) * 0.001f);
    // (The per-walk `mMaterialSync.clear()` that stood here is GONE — the memo
    // crosses frames now, validated by a fingerprint; see MaterialSync in the
    // header. It is PRUNED at the end of the walk instead, so it never holds a
    // material the document has dropped.)
    // THE SUN, RESOLVED ONCE FOR THE WHOLE WALK (clean-2 lane, 2026-09-13).
    // toLightDesc asks the document which directional is the sun, and the
    // document answers by building a QVector of every directional and SORTING
    // it (Scene::sunLight -> directionalLights) — that ran per directional
    // light per sync, at 60 Hz, to answer the same question with the same
    // answer. The light list cannot change inside one walk.
    // ...and with it the rest of the light-derived aggregates, which are a
    // fold over Scene::lights (bounded by the LIGHT count) rather than over the
    // walk (bounded by the NODE count) since the dirty set: a still frame runs
    // no walk at all, so nothing else could re-derive them. §3.7.
    refreshLightAggregates();
    // ...and the one light property that follows a TRANSFORM rather than an
    // edit: the sun's atmosphere tint (syncSunAtmosphere says why it cannot
    // ride the dirty set).
    syncSunAtmosphere();
    // MOBILITY (REALTIME_REFLECTIONS_SPEC §3.3): mMovableNodes and
    // mMovableLights are INCREMENTAL now (syncMobility maintains them on each
    // entry's transition, releaseEntry gives the contribution back) — only the
    // per-frame EVENT flag is reset here.
    mMobilityChanged = false;
    // THE PLAY EDGE the soft-promotion rule is scoped to. On the FALLING edge
    // the document has already cleared every node's soft flag
    // (Scene::setPlaying) and the transforms are back where the author left
    // them, so the warn latches and the remembered poses go with it.
    {
        const bool playing = mSource->isPlaying();
        if (playing != mWasPlaying) {
            for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
                it->posed = false;
                it->mobilityWarned = false;
            }
            mWasPlaying = playing;
            // A FULL-WALK TRIGGER (§3.5). The play edge clears soft mobility
            // tree-wide and re-arms the surprise-mover watch, and a node has to
            // be SEEN STANDING STILL before it can be seen moving — which means
            // every entry needs one visit on this frame, not whenever it next
            // happens to be written.
            mFullWalkPending = true;
        }
    }
    // (mHorizonFloor / mAnyRefractive / mAnyDistortion are no longer reset
    // here: the first is sticky and the other two are entry counts. A still
    // frame would have reset them to nothing and left them there — the
    // refraction pass turning off under a glass object nobody touched. §3.7.)
    // ---- THE CHOICE (SPECS/DIRTY_SET_MIRROR_SPEC.md, owner option A) ------
    //
    // The FULL WALK is the explicit, rare answer: a bind, a graph re-take, an
    // evacuation, the play edge, an eviction overflow, or the verification
    // mode. Every other frame the document hands over the list of what it
    // changed and the mirror handles that instead — which is why a still frame
    // now costs the bounded passes and nothing else.
    iris::SceneNode *root = mSource->getRootNode().data();
    iris::NodeDirtySet *marks = mSource->dirtySet();
    // An eviction list that overflowed (a scene nobody was mirroring built and
    // dropped thousands of nodes) is the one answer that cannot be reconciled
    // incrementally: walk everything and let the stamp sweep find the dead.
    if (marks && marks->takeOverflow()) mFullWalkPending = true;
    const bool full = mFullWalkPending || mVerifyEverything;
    mLastWalkWasFull = full;
    if (full) {
        // JAH_MIRROR_VERIFY=full: the walk IS the verification, so every
        // material is re-fingerprinted and every push it makes is a catch.
        if (mVerifyEverything && !mFullWalkPending) {
            mVerifying = true;
            mVerifyMaterialQuota = ~0u;
        }
        // THE LIST IS DISCARDED BEFORE THE WALK, not after: the walk is about
        // to look at every node, so nothing standing on it is worth visiting —
        // but the walk itself RAISES marks (the mirror's own soft-mobility
        // promotion, the users of a material whose shading model just
        // switched), and those belong to the NEXT sync. Clearing afterwards
        // would throw them away.
        if (marks) {
            marks->takeDirty(mDirtyScratch);
            for (iris::SceneNode *n : mDirtyScratch) if (n) n->_takeDirtyMask();
            mDirtyScratch.clear();
            // Evictions are safe to drop here: those nodes are out of the tree,
            // so the walk cannot stamp them and removeMissing releases them.
            marks->takeEvicted(mEvictedScratch);
            mEvictedScratch.clear();
        }
        // ...and the same for the material epoch: read BEFORE, so a material
        // touched DURING the walk is caught by the next sync rather than
        // assumed to have been seen.
        mMaterialRevision = iris::Material::globalRevision();
        // RAW children, no QList: SceneNode::children() builds a
        // QList<QSharedPointer> — a heap allocation plus an atomic refcount per
        // child — and this walk runs over the whole document.
        const std::size_t rootChildren = iris::graph::childCount(root->graphNode());
        const bool rootShown = root->isVisible();
        { MirrorStage s(mon, "mirror.walk");
        for (std::size_t i = 0; i < rootChildren; ++i)
            if (iris::SceneNode *c = iris::graph::ownerOf(iris::graph::childAt(root->graphNode(), i)))
                visit(c, rootShown, false);
        }
        { MirrorStage s(mon, "mirror.removeMissing");
        removeMissing();
        }
        mVerifying = false;
        mVerifyMaterialQuota = 0;
        mFullWalkPending = false;
    } else {
        // EVICTIONS FIRST (§3.4): an address a same-frame insert recycled must
        // be released before it is adopted again.
        { MirrorStage s(mon, "mirror.evict");
        consumeEvicted();
        }
        { MirrorStage s(mon, "mirror.walk");
        consumeDirty();
        }
    }
    // The handful of things that legitimately cost something EVERY frame, all
    // bounded by the scene's light and camera registries rather than by node
    // count — and idempotent, so running them after a full walk costs a latch
    // compare each.
    { MirrorStage s(mon, "mirror.perframe");
    syncPerFrameSet();
    }
    // THE CACHE SWEEP, ON DEMAND. reclaimUnused builds three QSets out of every
    // entry in the scene; at 10k nodes that was 20k+ set inserts a frame to
    // conclude, almost always, that nothing had been dropped. An engine mesh /
    // material / texture can only become unreferenced when an entry is released
    // or when an entry's reference to one CHANGES — every such site arms the
    // flag, and only then does the sweep run.
    if (mReclaimPending) { MirrorStage s(mon, "mirror.reclaim");
                           reclaimUnused(); mReclaimPending = false; }
    // LIVE TEXTURES (ADDENDUM A-1). AFTER the sweep, so a texture the sweep
    // just freed is not uploaded into; before the frame is drawn, because the
    // engine's upload records into the OPEN command buffer and therefore lands
    // ahead of this frame's draws with no flush of ours.
    { MirrorStage s(mon, "mirror.liveTextures");
    syncLiveTextures();
    }
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
    { MirrorStage s(mon, "mirror.skeletons");
    syncSkeletonSharing();
    }
    { MirrorStage s(mon, "mirror.clips");
    syncClips();
    }
    { MirrorStage s(mon, "mirror.helpers");
    syncHighlight();
    syncGrid();
    syncGroundHorizon();
    }
    // AFTER removeMissing: a rider deleted from the document is a dangling key
    // in the reconciler's map until its entry is released (see the function).
    { MirrorStage s(mon, "mirror.riders");
    sweepStaleRiders();
    }
    // (THE MATERIAL MEMO'S END-OF-SYNC SWEEP IS GONE. It was one pass over
    // every material in the scene, every frame — 8,404 of them on the render
    // review's lattice — to conclude, almost always, that none had left; and a
    // still frame runs no walk, so nothing would stamp them and the whole memo
    // would be thrown away once a frame. The memo is pruned by reclaimUnused
    // instead, which already drops a material's record WITH the material and
    // which only runs when something was really released.)

    // THE AMORTISED VERIFIER, last: the bounded passes above have already
    // brought the helpers up to date, so anything this finds is a change the
    // DOCUMENT failed to report. Off during a full walk (which just checked
    // everything) and during the verification mode (which is the whole walk).
    if (!full) { MirrorStage s(mon, "mirror.verify");
                 runVerifier(); }

    return mVisited;
}

// ---- THE DIRTY SET (SPECS/DIRTY_SET_MIRROR_SPEC.md, owner option A) --------
//
// Everything below is the other half of sync(): the change list's consumers.
// None of them knows anything the full walk does not — visitNode is shared, so
// "run the full walk after the dirty one and demand it pushes nothing" is a
// real oracle (verifyAgainstFullWalk) rather than two implementations that
// agree by luck.

/// The parent's EFFECTIVE visibility, read from the DOCUMENT rather than from
/// the parent's entry. O(depth), and order-free: whatever order the change
/// list happens to be in, a marked node resolves the same answer the walk would
/// have threaded down to it. (isVisibleInScene answers a socket rider through
/// its DOCUMENT parent, which is the same rule the walk applies.)
/// MEMOISED FOR THE SYNC. A thousand crates under twenty groups is twenty
/// distinct answers; without this it would be a thousand ancestor walks.
/// Nothing in the document moves during a sync, so the memo cannot go stale
/// inside one — the single exception is the mirror's own soft-mobility
/// promotion, whose descendants are marked by the cascade and are re-resolved
/// on the next sync anyway (the documented one-frame latency, §3.3).
static const quint8 kParentShown = 1, kParentMovable = 2, kParentKnown = 4;

quint8 SceneMirror::parentStateOf(iris::SceneNode *node)
{
    iris::SceneNode *parent = node ? iris::graph::ownerOf(iris::graph::parentOf(node->graphNode()))
                                   : nullptr;
    // A CHILD OF THE ROOT: the walk's own entry conditions — the root's flag is
    // the parentShown it threads down, and the root never resolves movable.
    if (!parent) {
        const bool rootShown = mSource && mSource->getRootNode()
                                   ? mSource->getRootNode()->isVisible() : true;
        return quint8(kParentKnown | (rootShown ? kParentShown : 0));
    }
    auto it = mParentState.constFind(parent);
    if (it != mParentState.constEnd()) return it.value();
    quint8 v = kParentKnown;
    if (parent->isVisibleInScene()) v |= kParentShown;
    if (parent->resolvedMobility() == iris::Mobility::Movable) v |= kParentMovable;
    mParentState.insert(parent, v);
    return v;
}

bool SceneMirror::parentShownOf(iris::SceneNode *node)
{
    return (parentStateOf(node) & kParentShown) != 0;
}

/// ...and its resolved mobility, the same way (rule 2 is an OR up the chain).
bool SceneMirror::parentMovableOf(iris::SceneNode *node)
{
    return (parentStateOf(node) & kParentMovable) != 0;
}

/// ONE MARKED NODE.
void SceneMirror::visitDirty(iris::SceneNode *node)
{
    if (!node) return;
    // THE ROOT IS NOT MIRRORED. The full walk starts at the root's CHILDREN —
    // the World node is the document's container and has no engine object of
    // its own — so a mark on it (addChild fires Structure on the parent as
    // well as on the child) must not adopt it. Its own `visible` flag still
    // reaches its children: setVisible cascades the whole subtree.
    if (mSource && node == mSource->getRootNode().data()) return;
    visitNode(node, parentShownOf(node), parentMovableOf(node));
}

/// Entries whose document nodes have left. This is what replaced
/// removeMissing()'s stamp sweep — a pass over EVERY entry in the scene, every
/// frame, to conclude that none had gone.
///
/// The pointers here are raw and their nodes may already be freed: nothing
/// dereferences one. The entry map is keyed by pointer, releaseEntry reads only
/// the entry, and mBoneRiders is removed by key.
void SceneMirror::consumeEvicted()
{
    if (!mSource) return;
    mSource->dirtySet()->takeEvicted(mEvictedScratch);
    if (mEvictedScratch.empty()) return;
    bool droppedRigged = false;
    for (iris::SceneNode *n : mEvictedScratch) {
        auto it = mEntries.find(n);
        if (it == mEntries.end()) continue;
        droppedRigged = droppedRigged || it->gpuSkinned;
        mBoneRiders.remove(it.key());
        releaseEntry(*it);
        mEntries.erase(it);
        ++mEvictedNodes;
    }
    mEvictedScratch.clear();
    // A character that lost a piece has a stale union cached against a host
    // node that may itself be gone (removeMissing's own note) — AND every
    // SURVIVING piece has to re-attach onto the rig that replaces it: the union
    // is derived from the pieces that are there, so one leaving changes the
    // bone list every other piece is skinned against (skeletal.union_rig S16).
    // The walk used to find that by visiting them all; the change list has to
    // be told, because nothing wrote those nodes.
    if (droppedRigged) {
        mCharacterRigs.clear();
        for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
            if (!it->gpuSkinned || !it->docNode) continue;
            // The epoch the entry remembers names a rig that no longer exists;
            // a cleared cache re-derives from zero and could hand out the same
            // number, so force the re-attach rather than compare to it.
            it->characterEpoch = ~quint32(0);
            // Onto the DOCUMENT's list, not this sync's scratch: consumeDirty
            // takes the document's list after this pass, so a mark made here
            // is consumed on the same frame.
            it->docNode->markChanged(iris::NodeChange::Content);
        }
    }
}

/// A MATERIAL EDIT MOVES NO NODE (§3.6), so the change list cannot see one.
/// The whole question is answered by ONE relaxed atomic read on a still frame;
/// only when some material in the process really was written does this look at
/// the memo, and only the materials whose own revision moved reach their nodes.
void SceneMirror::markChangedMaterials()
{
    const quint64 now = iris::Material::globalRevision();
    if (now == mMaterialRevision) return;
    mMaterialRevision = now;
    for (auto it = mMaterialSync.constBegin(); it != mMaterialSync.constEnd(); ++it) {
        iris::Material *m = it.key();
        if (!m || m->revision() == it->revision) continue;
        markMaterialUsersDirty(m);
    }
}

/// Queues a character's skinned pieces for a visit this sync.
void SceneMirror::markPiecesDirty(const QVector<iris::SceneNode *> &pieces)
{
    for (iris::SceneNode *n : pieces) {
        if (!n) continue;
        if (mConsumingDirty) mDirtyScratch.push_back(n);   // THIS sync, not the next
        else n->markChanged(iris::NodeChange::Content);
    }
}

void SceneMirror::markMaterialUsersDirty(iris::Material *material)
{
    if (!material) return;
    auto it = mMaterialUsers.constFind(material);
    if (it == mMaterialUsers.constEnd()) return;
    for (iris::SceneNode *n : it.value()) {
        if (!n) continue;
        if (mConsumingDirty) mDirtyScratch.push_back(n);   // THIS sync, not the next
        else n->markChanged(iris::NodeChange::Content);
    }
}

/// THE CHANGE LIST.
void SceneMirror::consumeDirty()
{
    if (!mSource) return;
    mParentState.clear();
    mSource->dirtySet()->takeDirty(mDirtyScratch);
    mDirtyNodes = quint64(mDirtyScratch.size());
    mConsumingDirty = true;
    // THE DOCUMENT'S OWN LIST FIRST, MATERIALS AFTER (lead review R2 #1).
    //
    // markChangedMaterials reads the REVISION of every material the memo holds
    // — a dereference of a raw pointer — and a material the document dropped
    // since the last sync is reachable there until something prunes it. The
    // node that dropped it is ON THIS LIST (setMaterial marks Content), and
    // visiting that node is what calls noteMaterialUser and takes the dead
    // material out of the memo. So the pass over the list is also the pass
    // that makes the material question safe to ask.
    //
    // ONE INDEX LOOP, because every phase APPENDS to the same list: a
    // shading-model switch queues every other node drawing that material (their
    // Items were rebuilt with the default query mask), a character's piece set
    // changing queues its other pieces, and the material pass queues the users
    // of every material whose revision moved. All of them have to be reached on
    // THIS frame. A node queued twice is visited twice, which is idempotent and
    // cheaper than a set.
    bool materialsAsked = false;
    for (std::size_t i = 0;; ++i) {
        if (i >= mDirtyScratch.size()) {
            if (materialsAsked) break;
            materialsAsked = true;
            markChangedMaterials();
            if (i >= mDirtyScratch.size()) break;
        }
        iris::SceneNode *n = mDirtyScratch[i];
        if (!n) continue;               // tombstoned: the node left the document
        // CLEARED BEFORE THE VISIT, so a write the visit itself makes (the
        // mirror's own soft-mobility promotion) is not lost.
        const quint16 mask = n->_takeDirtyMask();
        // JAH_MIRROR_TRACE=1 names what the document reported, per sync. The
        // one question this design makes hard to answer by reading code — "why
        // is anything on the list at all on a still frame?" — and the answer
        // found the first defect it looked for (the viewport re-asserts the
        // selection every frame, which was marking a node per frame forever).
        if (mTrace)
            qWarning("mirror.dirty: '%s' mask=0x%04x", qUtf8Printable(n->name), unsigned(mask));
        visitDirty(n);
    }
    mConsumingDirty = false;
    mDirtyScratch.clear();
}

/// WHAT LEGITIMATELY COSTS SOMETHING EVERY FRAME (§3.3).
///
/// Two kinds of thing, and both are bounded by the scene's own light and
/// camera registries — never by node count:
///
///  * a TRACKING camera's focus smoothing, which advances by the frame's dt and
///    is not a response to any document write at all;
///  * the light and camera HELPERS, whose geometry follows a WORLD transform:
///    a lamp on a moving car is not itself written when the car moves, and the
///    icon has to travel with it. The pushes are all latched, so a still frame
///    pays a hash of a short transform chain per light and nothing else — which
///    is exactly what the full walk paid for them before.
void SceneMirror::syncPerFrameSet()
{
    if (!mSource) return;
    for (const iris::LightNodePtr &light : mSource->lights) {
        if (!light) continue;
        auto it = mEntries.find(light.data());
        if (it == mEntries.end() || !it->node) continue;   // not adopted yet
        syncLightWires(*it, light.data());
        if (light->lightType == iris::LightType::Sky
            || !it->wireNode) continue;
        syncLightIcon(*it, light.data());
    }
    for (const iris::CameraNodePtr &cam : mSource->cameras) {
        if (!cam) continue;
        resolveFocusTracking(cam.data());
        auto it = mEntries.find(cam.data());
        if (it == mEntries.end() || !it->node) continue;
        syncCameraWires(*it, cam.data());
    }
}

/// THE AMORTISED VERIFIER (§3.8). `mVerifierBudget` entries a sync on a
/// rotating cursor: a full pass over an 8,404-node scene every ~2.2 s at 60 Hz,
/// for about a tenth of a millisecond a frame. Any push it makes is a change
/// the document did not report — counted, named once, and healed by the push
/// itself, so the screen catches up within one rotation instead of staying
/// wrong until something else happens to touch the node.
void SceneMirror::runVerifier()
{
    if (!mVerifierBudget || mEntries.isEmpty()) return;
    const quint64 count = quint64(mEntries.size());
    if (mVerifierCursor >= count) mVerifierCursor = 0;
    // THE WALK TO THE CURSOR IS O(cursor), and that is a deliberate choice, not
    // an oversight (the comment that stood here claimed otherwise — lead review
    // R2 #8). QHash has no random access and no stable iterator across the
    // insert/erase this map sees every time a node is adopted or released, so
    // the alternatives are a stored iterator that has to be invalidated at five
    // seams and re-found anyway, or this: skip to the cursor and take a
    // contiguous run. The skip is a pointer-chase per bucket with no work in it
    // — measured inside `mirror.verify`, which is 0.370 ms for the WHOLE pass
    // on an 8,404-entry map, budget included. The order is stable between
    // rehashes, and a rehash costs one rotation's coverage, never correctness:
    // the cursor wraps and every entry is reached again.
    quint64 i = 0;
    unsigned done = 0;
    mParentState.clear();
    mVerifying = true;
    mVerifyMaterialQuota = mVerifierMaterialBudget;
    for (auto it = mEntries.begin(); it != mEntries.end() && done < mVerifierBudget; ++it, ++i) {
        if (i < mVerifierCursor) continue;
        iris::SceneNode *n = it->docNode;
        ++done;
        if (!n) continue;
        visitNode(n, parentShownOf(n), parentMovableOf(n));
    }
    mVerifying = false;
    mVerifierCursor += done;
    if (done < mVerifierBudget) mVerifierCursor = 0;   // wrapped
}

/// THE ORACLE. Runs the whole walk without consuming the change list and
/// answers how many engine pushes it made — zero after a correct dirty sync.
quint64 SceneMirror::verifyAgainstFullWalk()
{
    if (!mSource || !mSource->getRootNode()) return 0;
    const quint64 before = mVisitPushes;
    iris::SceneNode *root = mSource->getRootNode().data();
    const std::size_t n = iris::graph::childCount(root->graphNode());
    const bool rootShown = root->isVisible();
    mVerifying = true;
    mVerifyMaterialQuota = ~0u;      // the ORACLE re-reads every material
    for (std::size_t i = 0; i < n; ++i)
        if (iris::SceneNode *c = iris::graph::ownerOf(iris::graph::childAt(root->graphNode(), i)))
            visit(c, rootShown, false);
    mVerifying = false;
    mVerifyMaterialQuota = 0;
    return mVisitPushes - before;
}

/// THE LIGHT-DERIVED AGGREGATES (§3.7), folded over Scene::lights.
///
/// The walk used to accumulate these as it passed each light: "is anything
/// casting", the strongest filter anyone asked for and the biggest atlas. A
/// still frame runs no walk, so the fold moves to the one enumeration that is
/// bounded by the LIGHT count instead of the node count — the scene's own light
/// registry, which holds exactly the lights the walk would have reached.
void SceneMirror::refreshLightAggregates()
{
    mSyncSun = mSource ? mSource->sunLight().data() : nullptr;
    mAnyShadowCaster = false;
    mShadowFilter = ShadowFilter::Hard;
    mMaxShadowResolution = 0;
    if (!mSource) return;
    for (const iris::LightNodePtr &lp : mSource->lights) {
        iris::LightNode *light = lp.data();
        if (!light) continue;
        // Exactly the walk's own test, in the walk's own order: a Sky Light
        // returns before the fold (it is the ambient, not a light), an Area
        // light can never cast, and None is not a request.
        if (light->lightType == iris::LightType::Sky) continue;
        if (light->lightType == iris::LightType::Area) continue;
        if (!light->shadowMap || light->shadowMap->shadowType == iris::ShadowMapType::None)
            continue;
        ShadowFilter f = ShadowFilter::Hard;
        if (light->shadowMap->shadowType == iris::ShadowMapType::Soft)          f = ShadowFilter::Soft;
        else if (light->shadowMap->shadowType == iris::ShadowMapType::VerySoft) f = ShadowFilter::VerySoft;
        if (!mAnyShadowCaster || int(f) > int(mShadowFilter)) mShadowFilter = f;
        mAnyShadowCaster = true;
        if (light->shadowMap->resolution > 0)
            mMaxShadowResolution = std::max(mMaxShadowResolution,
                                            unsigned(light->shadowMap->resolution));
    }
}

// THE SUN'S EFFECTIVE COLOUR, PUSHED WHEN THE SUN MOVES (SUN_FOLLOWS_
// ATMOSPHERE, lane ENGINE-7 item 6).
//
// The walk's light push is driven by the DIRTY SET, and a transform write does
// not mark a node: it does not have to, because the light rides the adopted
// node and the graph carries its direction (the push's own comment says so).
// The atmosphere tint is the one thing about a light that DOES depend on its
// transform — rotating the sun down to the horizon reddens it — so it needs a
// per-frame test of its own. It is over ONE light and the tint is memoised by
// the renderer, so a still sun costs a compare and pushes nothing.
void SceneMirror::syncSunAtmosphere()
{
    if (!mTarget || !mSource || !mSyncSun) return;
    if (!mSyncSun->followsAtmosphere) return;     // nothing to follow
    auto it = mEntries.find(mSyncSun);
    if (it == mEntries.end() || !it->node || !it->lightPushed) return;
    const LightDesc want = toLightDesc(mSyncSun, mSyncSun, true,
                                       atmosphereTintFor(mSyncSun, mSyncSun));
    if (want == it->lastLight) return;            // the still case, every frame
    if (mTarget->setLight(it->node, want)) {
        notePush(mSyncSun, "sun atmosphere tint");
        it->hasLight = true;
        it->lastLight = want;
    }
}

QStringList SceneMirror::mirroredNodeNames() const
{
    QStringList out;
    for (auto it = mEntries.constBegin(); it != mEntries.constEnd(); ++it)
        out << (it->docNode ? it->docNode->name : QStringLiteral("<released>"));
    out.sort();
    return out;
}

int SceneMirror::pushedVisibility(const iris::SceneNode *node) const
{
    if (!node) return -1;
    auto it = mEntries.constFind(node);
    return it == mEntries.constEnd() ? -1 : it->visiblePushed;
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
    // SELECTION CHANGES WHAT THE HELPERS DRAW (a selected light shows its
    // falloff volume; a selected camera's body takes the highlight colour) and
    // it is EDITOR state, so the document marks nothing. The mirror marks
    // instead — the nodes leaving the selection and the ones joining it, which
    // is a list of a handful and never the scene.
    //
    // ONLY ON A REAL CHANGE. The viewport re-asserts the selection EVERY FRAME
    // (it hands the mirror the same list again, as it has always done), so
    // marking unconditionally put one node on the change list per frame for as
    // long as anything was selected — a still scene that was never still. The
    // engine-side latches were already change-guarded; this is the same
    // discipline one level up.
    bool selectionMoved = nodes.size() != mHighlighted.size()
                          || primary.data() != mHighlightRequestedPrimary.data();
    if (!selectionMoved)
        for (int i = 0; i < nodes.size(); ++i)
            if (nodes[i].data() != mHighlighted[i].data()) { selectionMoved = true; break; }
    if (!selectionMoved) return;
    mHighlightRequestedPrimary = primary;
    for (const auto &n : mHighlighted) if (n) n->markChanged(iris::NodeChange::Flags);
    for (const auto &n : nodes)        if (n) n->markChanged(iris::NodeChange::Flags);
    mHighlighted.clear();
    mHighlightSet.clear();
    for (const auto &n : nodes) if (n) { mHighlighted.append(n); mHighlightSet.insert(n.data()); }
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
    // A SET, not a scan of the list (MIRROR_SCALE lane). It is asked per light,
    // per camera and per selected mesh on every walk, and a rubber-band select
    // over a big scene puts hundreds of nodes in the list.
    return node && mHighlightSet.contains(node);
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
        // The walk below skips a hidden node and its subtree; a member hidden
        // by an ANCESTOR is just as off screen (SceneNode::isVisibleInScene),
        // so it gets no outline either — an outline around nothing.
        if (!highlighted || !highlighted->isVisibleInScene()) continue;
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

// THE HELPER TOGGLES are EDITOR state, not document state, so nothing in the
// document marks when they move — and the per-frame helper pass reaches every
// light and camera anyway. The DECAL wires are the exception: they ride the
// node visit, so a toggle has to make the decals look again.
void SceneMirror::setLightWires(bool on)
{
    if (mLightWires == on) return;
    mLightWires = on;
    markDecalsDirty();
}

void SceneMirror::setCameraBodies(bool on)
{
    if (mCameraBodies == on) return;
    mCameraBodies = on;
}

/// Queues every decal node for a visit. Decal wires are drawn from the node
/// visit (they are the decal's SHAPE, derived from its own local scale), so the
/// helpers toggle has to reach them.
void SceneMirror::markDecalsDirty()
{
    if (!mSource) return;
    for (const iris::DecalNodePtr &d : mSource->decals)
        if (d) d->markChanged(iris::NodeChange::Flags);
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
        // LATCHED like the light wires' (MIRROR_SCALE lane): an engine
        // setNodeVisible walks the node's subtree, and this ran every frame for
        // every camera body in the scene whether it was already hidden or not.
        if (e.wireNode && e.wireVisible != 0) {
            mTarget->setNodeVisible(e.wireNode, false);
            e.wireVisible = 0;
            notePush(camera, "camera body off");
        }
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
            notePush(camera, "camera body");
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
    // ON CHANGE ONLY, the same discipline the light wires' scale push uses: a
    // camera nobody is scaling re-pushed this transform, and this visibility,
    // sixty times a second.
    const iris::Vec3 sc = camera->getLocalScale();
    quint64 xk = 1469598103934665603ull;
    mixFloat(xk, sc.x()); mixFloat(xk, sc.y()); mixFloat(xk, sc.z());
    if (!e.wireXformPushed || e.wireXformKey != xk) {
        mTarget->setNodeTransform(e.wireNode, jahshaka::engine::Vec3(), jahshaka::engine::Quat(),
                                  jahshaka::engine::Vec3(sc.x() > 1e-6f ? 1.0f / sc.x() : 1.0f,
                                                         sc.y() > 1e-6f ? 1.0f / sc.y() : 1.0f,
                                                         sc.z() > 1e-6f ? 1.0f / sc.z() : 1.0f));
        e.wireXformKey = xk;
        e.wireXformPushed = true;
        notePush(camera, "camera wire transform");
    }
    if (e.wireVisible != 1) { mTarget->setNodeVisible(e.wireNode, true); e.wireVisible = 1; notePush(camera, "camera wire visible"); }
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
        // Latched (MIRROR_SCALE lane): setNodeVisible is a subtree walk in the
        // engine and the grid's node has two children, so a hidden grid cost
        // three node writes a frame to stay hidden.
        if (mGridNode && mGridVisiblePushed != 0) {
            mTarget->setNodeVisible(mGridNode, false);
            mGridVisiblePushed = 0;
        }
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
    if (mGridVisiblePushed != 1) { mTarget->setNodeVisible(mGridNode, true); mGridVisiblePushed = 1; }
}

// ---- the ground's horizon ---------------------------------------------------
//
// Owner, 2026-09-13 (testing push #18): "should the default ground not also be
// infinite in the Grand Showroom 2? It seems cut off." It is: the default floor
// is a 100 m square (lane L3 cut it from 1024 m so a new project would stop
// voxelising a square kilometre) and Showroom 2's hall alone is 48 m, so flying
// out of the hall shows the floor end in mid-air with sky underneath.
//
// This is the floor's own material carried on to the horizon by ONE extra plane
// that belongs to the mirror, not to the document. The rationale for the shape,
// and the two rejected alternatives with their measurements, are on
// syncGroundHorizon's declaration in the header.
//
// THE NUMBERS. `kHorizonHalfExtent` is twice the editor camera's far clip
// (1000 m, EngineSceneViewport::createEditorCamera), so the plane's own edge is
// always beyond the far plane and what a user can see is the far plane's own
// distance horizon, in every direction, at every height -- never a corner and
// never an edge that can be flown to. `kHorizonSink` puts it just under the
// floor so the floor always wins where they overlap; 5 mm reads as 0.006
// degrees at the floor's 50 m edge, an order of magnitude under a pixel on a
// 1080-line view.
//
// THE UV MAP IS MEASURED, NOT ASSUMED (lead review, and it was a real defect:
// the first cut used a hand-picked 0.0625 UV per metre with no offset and got
// the right DENSITY with the wrong PHASE and the wrong V SIGN -- the geometry
// edge came back as a texture seam, misregistered by 0.195 of a repeat and
// cycling along the north/south edges because v was mirrored). `fitGroundUvMap`
// fits u = ux*x + uc and v = vz*z + vc over the floor's OWN engine-side
// vertices -- the very arrays its Item is drawn from, after toMeshData's V flip
// -- so the horizon inherits ground.obj's density, offset and sign whatever the
// importer did with them, and survives a re-stage of that model. Measured on
// the shipped ground.obj: the FILE carries u = 0.0625x + 0.048828 and
// v = -0.0625z + 0.048828 (a non-zero offset, v running against +z), and the
// importer's flip (v = 1 - v) turns the second into v = 0.0625z + 0.951172.
// The material's own textureScale multiplies both meshes alike on top of it.
//
// WHAT THE HELPER BIT COSTS, stated rather than discovered later: kHelperBit
// keeps the horizon out of the reflection-probe captures and the planar
// reflection pass, so a mirror surface reflects the floor ENDING at its own
// 100 m edge with sky beyond. Indoors -- where every planar reflector and
// probe in the shipped content lives -- it cannot be seen; widening a capture
// mask to fix it would put a 4 km plane into every probe face, which is the
// worse trade.
static constexpr float kHorizonHalfExtent = 2000.0f;
static constexpr float kHorizonUvPerMetre = 0.0625f;   // the fallback density only
static constexpr float kHorizonSink       = 0.005f;

// The floor's own UV map, fitted over its engine-side vertices. False when the
// mesh carries no usable map (no UVs, or a degenerate one) -- the caller then
// falls back to the shipped density with no offset, which is the old behaviour
// and is only ever reached by a floor whose mesh is not ground.obj.
bool SceneMirror::fitGroundUvMap(iris::Mesh *mesh, float &ux, float &uc, float &vz, float &vc)
{
    if (!mesh) return false;
    MeshData data;
    if (!toMeshData(mesh, data)) return false;
    const size_t nv = data.positions.size() / 3;
    if (nv < 3 || data.uvs.size() != nv * 2) return false;
    // Least squares, one axis at a time: the floor is a plane in XZ, so u is a
    // function of x alone and v of z alone. A fit that does not describe the
    // mesh (a floor whose map is rotated, or per-face) is REFUSED on its
    // residual rather than half-applied.
    auto axis = [&](size_t posOff, size_t uvOff, float &k, float &c) {
        double sa = 0, sb = 0;
        for (size_t i = 0; i < nv; ++i) { sa += data.positions[i*3 + posOff]; sb += data.uvs[i*2 + uvOff]; }
        const double ma = sa / double(nv), mb = sb / double(nv);
        double num = 0, den = 0;
        for (size_t i = 0; i < nv; ++i) {
            const double da = data.positions[i*3 + posOff] - ma;
            num += da * (data.uvs[i*2 + uvOff] - mb);
            den += da * da;
        }
        if (!(den > 1e-6)) return false;
        k = float(num / den);
        c = float(mb - (num / den) * ma);
        double worst = 0;
        for (size_t i = 0; i < nv; ++i)
            worst = std::max(worst, std::abs(double(k) * data.positions[i*3 + posOff] + double(c)
                                             - data.uvs[i*2 + uvOff]));
        return worst < 1e-3 && std::abs(k) > 1e-9f;
    };
    return axis(0, 0, ux, uc) && axis(2, 1, vz, vc);
}

void SceneMirror::syncGroundHorizon()
{
    const iris::MeshNode *floor = mHorizonFloor;
    // A scene with no default floor (a thumbnail scene, a preview, a project
    // whose floor was deleted) has no horizon, and a hidden floor takes its
    // horizon with it.
    const bool want = floor && floor->isVisibleInScene();
    if (!want) {
        if (mHorizonNode && mHorizonVisible != 0) {
            mTarget->setNodeVisible(mHorizonNode, false);
            mHorizonVisible = 0;
        }
        // Let go of the floor's material as well: while the horizon holds one
        // the cache sweep keeps it alive (reclaimUnused), and a floor that has
        // been deleted must take its material with it.
        if (mHorizonMaterial) {
            mTarget->detachMesh(mHorizonNode);
            mHorizonMaterial = 0;
            mReclaimPending = true;
        }
        return;
    }

    if (!mHorizonNode) {
        mHorizonNode = mTarget->createNode();
        if (!mHorizonNode) return;
        // AN EDITOR HELPER, in the engine's sense (EnginePrivate.h's bit
        // scheme): kHelperBit instead of kVisibleBit takes the plane out of
        // every reflection-probe capture, out of the shadow nodes (nothing this
        // size may ever be a shadow caster or the atlas fits the horizon
        // instead of the scene) and out of kGiGeometryBit, while the main chain
        // -- which sets no visibility mask at all -- goes on drawing it.
        mTarget->setNodeHelper(mHorizonNode, true);
    }
    // THE FLOOR'S MESH decides the horizon's UV map, so a floor that changes
    // mesh rebuilds it (the map is measured off that mesh, above).
    iris::Mesh *floorMesh = floor->mesh.data();
    if (mHorizonMesh && floorMesh != mHorizonMeshSource) {
        mTarget->detachMesh(mHorizonNode);
        mTarget->destroyMesh(mHorizonMesh);
        mHorizonMesh = 0;
        mHorizonMaterial = 0;
        mReclaimPending = true;
    }
    if (!mHorizonMesh) {
        // Four corners, wound to face UP and no other way. The single winding is
        // load-bearing: the default floor is invisible from below (its
        // datablock culls back faces), and a horizon that was not would hide
        // whatever a camera dipping under y = 0 was looking at — which is not
        // an exotic pose at all, it is what editor.frameNode does whenever it
        // frames a small object from above (five pixel suites caught exactly
        // that: the camera lands at y = -0.09 and the subject went grey).
        //
        // The UVs come from the FLOOR'S OWN map (fitGroundUvMap) so the checker
        // crosses the floor's edge in phase; only a floor whose mesh has no
        // usable map falls back to the shipped density.
        float ux = kHorizonUvPerMetre, uc = 0.0f, vz = kHorizonUvPerMetre, vc = 0.0f;
        if (!fitGroundUvMap(floorMesh, ux, uc, vz, vc)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                irisLog("SceneMirror: the default floor's mesh carries no linear UV map — the "
                        "horizon falls back to the shipped checker density and may not line up");
            }
        }
        const float h = kHorizonHalfExtent;
        const float u0 = -h * ux + uc, u1 = h * ux + uc;
        const float v0 = -h * vz + vc, v1 = h * vz + vc;
        MeshData quad;
        quad.positions = { -h, 0.0f, -h,   h, 0.0f, -h,   h, 0.0f, h,   -h, 0.0f, h };
        quad.normals   = { 0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f,
                           0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f };
        quad.uvs       = { u0, v0,   u1, v0,   u1, v1,   u0, v1 };
        quad.indices   = { 0, 2, 1,  0, 3, 2 };
        mHorizonMesh = mTarget->createMesh(quad);
        if (!mHorizonMesh) return;
        mHorizonMeshSource = floorMesh;
        mHorizonMaterial = 0;      // nothing is attached yet
    }

    // THE FLOOR'S OWN MATERIAL, by id: every edit a user makes to the floor --
    // its colour, its checker, its tiling, a whole library material dropped on
    // it -- reaches the horizon with no work here, because both items point at
    // the same datablock. Only a material SWAP re-attaches.
    const MaterialId mat = materialFor(floor->material.data());
    if (mat && mat != mHorizonMaterial) {
        mTarget->attachMesh(mHorizonNode, mHorizonMesh, mat);
        mHorizonMaterial = mat;
        // ARM THE SWEEP. reclaimUnused runs BEFORE this stage and keeps the id
        // the horizon was holding alive (it is not an entry, so the sweep can
        // only see it through that pin); dropping the old one here without
        // re-arming leaves the previous datablock alive until something
        // unrelated arms the sweep — a library material dropped on the floor
        // leaked exactly that (lead review).
        mReclaimPending = true;
    }
    if (!mHorizonMaterial) return;

    // The floor's world transform, sunk. Rotation and scale ride along so a
    // scaled or tilted floor keeps its horizon attached to it (and its checker
    // density, which the scale multiplies on both meshes alike).
    //
    // AND NOTHING AT REST. The claim used to be "three pointer tests"; the line
    // below it resolved the floor's DERIVED WORLD TRANSFORM every frame, which
    // walks the node's parent chain (MIRROR_SCALE lane). The document's global
    // transform-write counter answers "can the floor have moved?" without
    // asking the graph anything: no write anywhere, no new world.
    const unsigned long long writes = iris::graph::transformWrites();
    if (writes == mHorizonWrites && mHorizonVisible == 1) return;
    mHorizonWrites = writes;
    const iris::Mat4 world = const_cast<iris::MeshNode *>(floor)->getGlobalTransform();
    if (world == mHorizonWorld && mHorizonVisible == 1) return;
    mHorizonWorld = world;
    const iris::Vec3 cx = world.column(0).toVector3D(), cy = world.column(1).toVector3D(),
                     cz = world.column(2).toVector3D();
    const iris::Vec3 scale(cx.length(), cy.length(), cz.length());
    const iris::Vec3 pos = world.column(3).toVector3D();
    const float sx = scale.x() > 1e-8f ? scale.x() : 1.0f, sy = scale.y() > 1e-8f ? scale.y() : 1.0f,
                sz = scale.z() > 1e-8f ? scale.z() : 1.0f;
    float m[9] = { cx.x() / sx, cy.x() / sy, cz.x() / sz,
                   cx.y() / sx, cy.y() / sy, cz.y() / sz,
                   cx.z() / sx, cy.z() / sy, cz.z() / sz };
    const iris::Quat rot = iris::Quat::fromRotationMatrix(iris::Mat3(m));
    mTarget->setNodeTransform(mHorizonNode,
                              Vec3(pos.x(), pos.y() - kHorizonSink * sy, pos.z()),
                              Quat(rot.x(), rot.y(), rot.z(), rot.scalar()),
                              Vec3(scale.x(), scale.y(), scale.z()));
    if (mHorizonVisible != 1) {
        mTarget->setNodeVisible(mHorizonNode, true);
        mHorizonVisible = 1;
    }
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
            // Latched, like the grid above: two subtree walks a frame to keep
            // two boxes nobody asked for hidden.
            if (mGiVolLitNode && mGiVolLitVisible != 0) {
                mTarget->setNodeVisible(mGiVolLitNode, false); mGiVolLitVisible = 0;
            }
            if (mGiVolProbeNode && mGiVolProbeVisible != 0) {
                mTarget->setNodeVisible(mGiVolProbeNode, false); mGiVolProbeVisible = 0;
            }
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
                             Vec3 &cachedMin, Vec3 &cachedMax, bool have, int &vis) {
        if (!have) { if (vis != 0) { mTarget->setNodeVisible(node, false); vis = 0; } return; }
        if (!mesh || !sameBox(mn, mx, cachedMin, cachedMax)) {
            if (mesh) { mTarget->detachMesh(node); mTarget->destroyMesh(mesh); mesh = 0; }
            mesh = mTarget->createLineMesh(boxEdges(mn, mx), false);
            if (mesh) mTarget->attachMesh(node, mesh, material);
            cachedMin = mn; cachedMax = mx;
        }
        const int want = mesh != 0 ? 1 : 0;
        if (vis != want) { mTarget->setNodeVisible(node, want != 0); vis = want; }
    };
    rebuild(mGiVolLitNode, mGiVolLitMesh, mGiVolLitMaterial,
            st.boundsMin, st.boundsMax, mGiVolLitMin, mGiVolLitMax, haveLit, mGiVolLitVisible);
    rebuild(mGiVolProbeNode, mGiVolProbeMesh, mGiVolProbeMaterial,
            st.probeRegionMin, st.probeRegionMax, mGiVolProbeMin, mGiVolProbeMax, haveProbe, mGiVolProbeVisible);
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
        notePush(e.docNode, "wire colour");
        e.wireColour = c;
        e.wireColourPushed = true;
    }
}

void SceneMirror::syncLightWires(Entry &e, iris::LightNode *light)
{
    if (!mLightWires) {
        // Hides the wire lines AND the icon billboard set riding on wireNode
        // (the engine toggles a set's visibility flags with its owning node).
        if (e.wireNode && e.wireVisible != 0) {
            mTarget->setNodeVisible(e.wireNode, false); e.wireVisible = 0;
            notePush(light, "light wires off");
        }
        return;
    }
    // A SKY LIGHT HAS NO SHAPE: it is the sky, everywhere. Icon only, no wires
    // (SKY_LIGHT_SPEC.md §2) — there is no falloff volume, no direction arrow
    // and no emitter rectangle to draw.
    if (light->lightType == iris::LightType::Sky) {
        if (!e.wireNode) {
            e.wireNode = mTarget->createNode(e.node);
            if (e.wireNode) mTarget->setNodeHelper(e.wireNode, true);
        }
        if (!e.wireNode) return;
        if (e.wireKind != -1) { mTarget->detachMesh(e.wireNode); e.wireKind = -1; notePush(light, "sky wire"); }
        if (e.wireVisible != 1) { mTarget->setNodeVisible(e.wireNode, true); e.wireVisible = 1; notePush(light, "sky wire visible"); }
        syncLightIcon(e, light);
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
        if (e.wireKind != -1) { mTarget->detachMesh(e.wireNode); e.wireKind = -1; notePush(light, "wire shape"); }
        // the icon set rides this node
        if (e.wireVisible != 1) { mTarget->setNodeVisible(e.wireNode, true); e.wireVisible = 1; notePush(light, "wire visible"); }
        syncLightIcon(e, light);
        return;
    }
    MeshId m = wireMeshFor(shape);
    if (!m) return;
    if (!e.wireMaterial) e.wireMaterial = mTarget->createUnlitMaterial(Colour(1, 1, 1), false);
    if (!e.wireMaterial) return;
    if (e.wireKind != shape) { if (mTarget->attachMesh(e.wireNode, m, e.wireMaterial)) { e.wireKind = shape; notePush(light, "wire shape"); } }
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
        notePush(light, "wire transform");
    }
    if (e.wireVisible != 1) { mTarget->setNodeVisible(e.wireNode, true); e.wireVisible = 1; notePush(light, "wire visible"); }
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
        // The Sky Light borrows the sun glyph's bundled image rather than
        // shipping a ninth icon nobody drew: it reads as "light from the sky",
        // and the panel/outliner name is what tells the two apart.
        case iris::LightType::Sky:         path = QStringLiteral(":/icons/light.png"); break;
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
        notePush(light, "light icon");
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

// ONE NODE, GIVEN ITS PARENT'S ANSWERS (DIRTY_SET_MIRROR_SPEC §4).
//
// This is the old visit()'s body with the child recursion lifted out of it and
// nothing else moved: the three callers — the full walk, one marked node, and
// the verifier — share every latch, which is what makes "run the full walk
// after the dirty one and demand it pushes nothing" a real oracle rather than
// two implementations agreeing by luck.
SceneMirror::VisitResult SceneMirror::visitNode(iris::SceneNode *node, bool parentShown,
                                                bool parentMovable)
{
    VisitResult out;
    if (!node) { out.descend = false; return out; }
    if (mVerifying) ++mVerifierVisits;
    else            ++mVisited;

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
        if (!graphNode) { out.descend = false; return out; }
        e.node = mTarget->adoptNode(const_cast<void *>(graphNode));
        e.graphNode = graphNode;
        e.graphEpoch = node->graphEpoch();
        if (!e.node) { out.descend = false; return out; }
        e.visiblePushed = -1;      // force one visibility application
        e.pickablePushed = -1;     // ...and one query-flag application
        e.lightMaskEverPushed = false;   // ...and one lighting-channel application
    }

    e.docNode = node;
    // EFFECTIVE VISIBILITY (RENDER_PIPELINE_AUDIT 1.1/1.2): what reaches the
    // engine is this node's own flag AND every ancestor's — the DOCUMENT's
    // rule, SceneNode::isVisibleInScene, computed here for one AND because the
    // walk is parent-first. It used to push the node's own flag and lean on
    // Ogre's setVisible cascade for the rest, which (1) cleared the GI bit of
    // the hidden node alone, so a hidden model root left every child
    // voxelised and bouncing light, and (2) on the way back set EVERY
    // descendant visible, re-revealing children the user had hidden
    // themselves — their own latch never moved, so nothing re-pushed them.
    // Pushing the effective state per node also covers what no engine-side
    // cascade can see: a socket rider (whose engine parent is a bone, not its
    // document parent) and a re-parent under a hidden or a visible node. Each
    // node's own flag is untouched; pushed on CHANGE only, like every other
    // signature-guarded half of this walk.
    const bool shown = parentShown && node->visible;
    const int wantVisible = shown ? 1 : 0;
    if (e.visiblePushed != wantVisible) {
        // THROUGH THE PARENT-FIRST VERB (ledger 179): this walk knows
        // `parentShown` — it is the argument — and the plain setNodeVisible
        // would derive the same answer again by walking up to the nearest
        // registered ancestor, one registry lookup per push. On a first sync
        // that is one per adopted node, for a value sitting in a local.
        mTarget->setNodeVisibleUnder(e.node, shown, parentShown);
        e.visiblePushed = wantVisible;
        notePush(node, "visibility");
    }

    // MOBILITY (REALTIME_REFLECTIONS_SPEC §3.3). Resolved here, where the walk
    // is parent-first and the parent's answer is already in hand — the same
    // shape effective visibility uses, and for the same reason: rule 2 ("it
    // travels with its parent") is an AND down the chain, not a per-node
    // question. `movable` is what this node's children inherit.
    //
    // BEFORE THE GEOMETRY, deliberately (lane R2): the renderer chooses an
    // item's render channel and its GI class when the item is CREATED, so a
    // node classified before its mesh attaches is born movable and costs
    // nothing. Pushed after the attach instead, every newly added moving object
    // — and every object in a scene being opened — would be created as still
    // world, joined to the voxel bounce, and then taken out of it again: one
    // from-scratch GI rebuild each, at load time.
    const bool movable = syncMobility(e, node, parentMovable);
    out.shown = shown;
    out.movable = movable;

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
        notePush(node, "lightMask");
    }

    // PER-OBJECT SHADOW CASTING. `SceneNode::castShadow` has been in the
    // document — serialized, reflected, set to false by the default floor —
    // since long before the engine had anywhere to put it, and NOTHING pushed
    // it: the flag was inert for years and the floor it was set on went on
    // casting into every lamp. This is the wire (SUN_AND_LIGHT_DEFAULTS §2.4).
    // Same shape as the mask above: change-guarded, the engine remembers it
    // across Item rebuilds, and a light node carries no Item so it is a no-op
    // there.
    const int wantCastShadow = node->getShadowCastingEnabled() ? 1 : 0;
    if (e.castShadowPushed != wantCastShadow) {
        mTarget->setNodeCastShadow(e.node, wantCastShadow != 0);
        e.castShadowPushed = wantCastShadow;
        notePush(node, "castShadow");
    }

    if (node->getSceneNodeType() == iris::SceneNodeType::Mesh) {
        auto *meshNode = static_cast<iris::MeshNode *>(node);
        // THE SCENE'S DEFAULT FLOOR, remembered for syncGroundHorizon. Recorded
        // here rather than searched for afterwards: the walk is already at every
        // mesh node, and the flag is the document's own (never the name).
        // STICKY, not re-found per walk (§3.7): a still frame runs no walk at
        // all, so the pointer has to survive one. It is dropped when this node
        // stops being the floor and when its entry is released.
        if (meshNode->defaultFloor) { if (!mHorizonFloor) mHorizonFloor = meshNode; }
        else if (mHorizonFloor == meshNode) mHorizonFloor = nullptr;
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
            // ONE memo probe for the whole branch — materialFor reads the same
            // entry, and the reference stays valid because nothing between here
            // and syncTextures inserts another material.
            const MaterialSync &attachMs = materialSyncFor(material);
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
                        e.clipSignature = 0;         // force a clip re-attach
                        e.shareEligibleValid = false;   // ...and re-decide sharing (rigId moved)
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
                notePush(node, "mesh attach");
                if (e.materialPtr != material) noteMaterialUser(node, e.materialPtr, material);
                e.hasMesh = true; e.material = mat; e.materialPtr = material; e.mesh = m; e.meshPtr = mesh;
                mReclaimPending = true;   // the old mesh/material may now be unreferenced
                e.texturesPushed = false;
                e.shadingModelPushed = -1;   // a NEW engine material may be in either family
                e.pickablePushed = -1;   // a NEW Item carries the default query mask
                syncTextures(e, attachMs);
                // The refractive/distortion contribution is this entry's from
                // the frame it attaches, not the frame after: the aggregate is
                // incremental now and nothing re-folds the scene (§3.7).
                if (attachMs.hasPbr)
                    noteRefractive(e, attachMs.pbr.alphaMode == PbrAlphaMode::Refractive,
                                   attachMs.pbr.shadingModel == ShadingModel::Distortion);
            }
        } else if (!mesh && e.hasMesh) {
            // The document dropped the mesh (a node kept, its MeshPtr cleared).
            // Without this the engine kept drawing the old geometry forever.
            mTarget->detachMesh(e.node);
            notePush(node, "mesh detach");
            noteMaterialUser(node, e.materialPtr, nullptr);
            e.materialPtr = nullptr;
            noteRefractive(e, false, false);
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
        }
        // NOT `else if` (DIRTY_SET_MIRROR_SPEC): the attach above resets
        // `shadingModelPushed` to -1 because a NEW engine material may be in
        // either family, and the push that answers that used to happen on the
        // NEXT frame's walk. There is no next frame now — so an attached node
        // whose material asked for Unlit or Distortion rendered Lit forever.
        // The memo makes this free on the attach frame (it was validated a few
        // lines up and is keyed on the same sync stamp).
        if (e.hasMesh && e.material && material) {
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
                    if (mTarget->setShadingModel(e.material, ms.pbr.shadingModel)) {
                        notePush(node, "shading model");
                        onMaterialItemsRebuilt(e.material);
                        // ...and every OTHER node drawing this material owes a
                        // query-flag re-push (its Items were rebuilt too). The
                        // walk used to reach them by walking; the change list
                        // has to be told (§3.6).
                        markMaterialUsersDirty(material);
                    }
                }
                // ONE COMPARE PER MATERIAL, not per node. See PbrPush in the
                // header for what this replaced and why it mattered.
                // ONE COMPARE PER MATERIAL, not per node — and it is the
                // fingerprint the memo computed anyway, not a second full
                // PbrParams compare through a second hash.
                MaterialSync &push = const_cast<MaterialSync &>(ms);
                if (!push.pushed || push.pushedTo != e.material
                    || push.pushedFingerprint != ms.fingerprint) {
                    if (mTarget->setPbrMaterial(e.material, ms.pbr)) {
                        notePush(node, "pbr params");
                        push.pushedTo = e.material;
                        push.pushedFingerprint = ms.fingerprint;
                        push.pushed = true;
                    }
                }
                noteRefractive(e, ms.pbr.alphaMode == PbrAlphaMode::Refractive,
                               ms.pbr.shadingModel == ShadingModel::Distortion);
            }
            syncTextures(e, ms);
        }
        // How many pieces of a multi-piece character the walk has seen — AFTER
        // the attach above, so a piece counts on the frame it is rigged rather
        // than the one after. Below two, syncSkeletonSharing has nothing to do
        // and returns without touching an entry (it used to iterate every entry
        // in the scene, every frame, to find that out).
        noteRigCounts(e);
    }

    // PICKING'S QUERY FLAGS, AFTER the geometry (DIRTY_SET_MIRROR_SPEC).
    //
    // It used to stand above the mesh branch, which was harmless only because
    // the walk came back next frame: an ATTACH resets `pickablePushed` to -1
    // (a new Item carries Ogre's default query mask) and a shading-model switch
    // bumps the material's item serial, and BOTH happen below — so the flag
    // went out one frame late. With the dirty set there is no next frame: a
    // node nobody writes again is never visited again, and an unpickable object
    // stayed clickable for the life of the scene. Ordering it after the branch
    // that invalidates it is the whole fix.
    //
    // Picking's broad phase is Ogre's RaySceneQuery (SCENEGRAPH_SPEC §2) and
    // its mask is tested inside the SIMD sweep, so `pickable` has to reach the
    // node's engine objects as QUERY FLAGS. Change-guarded; the document's flag
    // stays the authority and is re-checked exactly on the candidates.
    {
        const int wantPickable = node->isPickable() ? 1 : 0;
        const quint32 wantItemSerial = e.material ? mMaterialItemSerial.value(e.material, 0) : 0;
        if (e.pickablePushed != wantPickable || e.materialItemSerial != wantItemSerial) {
            iris::graph::setPickable(node->graphNode(), wantPickable != 0);
            e.pickablePushed = wantPickable;
            e.materialItemSerial = wantItemSerial;
            notePush(node, "pickable");
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
            notePush(node, "planar reflector");
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
            notePush(node, "gi bounds");
        }
    }

    if (node->getSceneNodeType() == iris::SceneNodeType::ParticleSystem) {
        syncParticles(e, static_cast<iris::ParticleSystemNode *>(node));
    } else if (e.hasParticles) {
        // A node may stop being an emitter without being removed (the document
        // changes a node's type in place). removeParticleSystem is the explicit
        // counterpart setParticleSystem needs for exactly that.
        mTarget->removeParticleSystem(e.node);
        notePush(node, "particles removed");
        e.hasParticles = false;
        e.particleSignature = 0;
        e.particleTexture = 0;
        mReclaimPending = true;
    }

    if (node->getSceneNodeType() == iris::SceneNodeType::Light) {
        // The light rides on the mirrored node: position and direction follow the document.
        auto *light = static_cast<iris::LightNode *>(node);
        // A SKY LIGHT IS NOT AN Ogre::Light (SKY_LIGHT_SPEC.md §2). It has no
        // position, no direction, no range and casts nothing: it is the scene's
        // ambient, pushed once per change through setAmbientSh in
        // applyEnvironment. Nothing about it belongs in the forward light list,
        // and creating one would cost a light slot per pass for a term the
        // shader already has. The NODE still exists (the icon is pickable and
        // selectable like any other light's) — only the engine light is absent.
        if (light->lightType == iris::LightType::Sky) {
            if (e.hasLight) {
                mTarget->removeLight(e.node);
                notePush(node, "sky light has no engine light");
                e.hasLight = false;
            }
            e.lightPushed = false;
            syncLightWires(e, light);
            syncLightIcon(e, light);
            // THE EARLY RETURN IS GONE (lead review R2 #6). It used to skip the
            // rest of the visit AND the child recursion, which made a Sky
            // Light's children invisible to the full walk — removeMissing then
            // released their entries every frame — while the dirty pass, which
            // reaches a marked node directly and resolves its parent from the
            // DOCUMENT, adopted them. Two modes disagreeing about the same
            // scene is the one thing this lane cannot ship. A Sky Light's
            // children are ordinary nodes; the sky-specific work above it (no
            // engine light, the icon, no wires) is what makes it a Sky Light.
            //
            // Nothing below can misfire on it: it carries no mesh, no
            // particles, no decal and is not a camera, and the light branch
            // itself has already been answered.
        } else {
            // ON CHANGE ONLY (audit F7). setLight is ~20 Ogre setters — type,
            // diffuse, specular, cast-shadows, power scale, an attenuation solve
            // (setAttenuationBasedOnRadius takes a square root and rewrites the
            // light's local AABB), spot range — plus two std::string compares for
            // the profile/mask paths, and it ran for every light in the scene on
            // every frame to re-push values a human edits by hand. It reads NOTHING
            // from the node's transform (the light rides the adopted node and the
            // graph carries position and direction), so skipping an unchanged push
            // cannot freeze a moving light.
            const LightDesc want = toLightDesc(light, mSyncSun, true,
                                               atmosphereTintFor(light, mSyncSun));
            // By value (LightDesc::operator==, beside the struct — every field
            // setLight reads is in it, which is what keeps a new field from
            // silently stopping at the first push).
            if (!e.lightPushed || want != e.lastLight) {
                if (mTarget->setLight(e.node, want)) {
                    notePush(node, "light");
                    e.hasLight = true;
                    e.lastLight = want;
                    e.lightPushed = true;
                }
            }
            // (The per-light shadow fold that stood here — strongest filter, largest
            // atlas request, "does anything cast" — is refreshLightAggregates()
            // now: it is a fold over Scene::lights, which is BOUNDED BY THE LIGHT
            // COUNT and therefore free, and a still frame runs no walk to fold it
            // over. Same answer, same order, once a sync.)
            syncLightWires(e, light);
        }
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
        // FOCUS TRACKING IS NOT PART OF THE VISIT any more: it advances a
        // SMOOTHING FILTER by the frame's dt, so running it twice in one frame
        // (a dirty visit and then the verifier's oracle walk over the same
        // camera) would double the travel. It belongs with the other things
        // that legitimately cost something every frame — syncPerFrameSet().
        syncCameraWires(e, cam);
    }

    return out;
}

// THE FULL WALK. It runs at the explicit triggers of DIRTY_SET_MIRROR_SPEC §3.5
// — a bind, a graph re-take, an evacuation, the play edge, an eviction
// overflow — and as the verifier's oracle. Every other frame the mirror handles
// the change list instead (consumeDirty).
void SceneMirror::visit(iris::SceneNode *node, bool parentShown, bool parentMovable)
{
    const VisitResult r = visitNode(node, parentShown, parentMovable);
    if (!r.descend || !node) return;
    // `e` is a reference into a QHash and the recursion INSERTS entries, which
    // QHash does not keep value references stable across (read-after-destroy under
    // ASan) — which is why visitNode hands its answers back BY VALUE rather
    // than leaving the caller holding one.
    const iris::graph::NodeHandle h = node->graphNode();
    const std::size_t n = iris::graph::childCount(h);
    for (std::size_t i = 0; i < n; ++i)
        if (iris::SceneNode *c = iris::graph::ownerOf(iris::graph::childAt(h, i)))
            visit(c, r.shown, r.movable);
}

// ---- mobility -------------------------------------------------------------------
// SPECS/REALTIME_REFLECTIONS_SPEC.md §3.3. The DOCUMENT decides "does this move?"
// and the engine is told; lane R1 pushes and counts, lane R2 spends it (movable
// objects out of the reflection probes and the GI geometry set, into the view
// and planar shadow maps only).
//
// TWO THINGS HAPPEN HERE and they must not be confused:
//
//  * THE RESOLUTION is PREDICTIVE. It reads drivers — physics, an avatar, a
//    socket, a playing clip, a rig with a clip, a particle emitter — and the
//    parent's answer. It never reads "did this move", because flipping an
//    object's GI class costs a from-scratch GI rebuild, and an editor drag that
//    promoted would put that rebuild in the middle of the gesture.
//
//  * THE SOFT PROMOTION is the one place movement IS read, and only while the
//    document is PLAYING (owner decision O3). A script pushing a prop nobody
//    marked Movable gets treated as movable from that frame — no rebuild, so
//    the bounce light it left behind stays as a ghost until play stops — plus
//    one warning naming the object. Once per node per play session: the latch
//    is the entry's, and the whole set is cleared on both play edges.
bool SceneMirror::syncMobility(Entry &e, iris::SceneNode *node, bool parentMovable)
{
    iris::MobilityReason why = iris::MobilityReason::Default;
    bool movable = node->resolveMobility(parentMovable, &why) == iris::Mobility::Movable;

    // THE SURPRISE MOVER. Only for a node that resolved static with nobody
    // having said so (an explicit `static` is a decision we keep honouring, and
    // its ghost is the author's own choice), and only while playing.
    if (!movable && mSource && mSource->isPlaying() && node->mobility() == iris::Mobility::Auto) {
        const iris::Vec3 p = node->getLocalPos();
        const iris::Quat r = node->getLocalRot();
        const iris::Vec3 sc = node->getLocalScale();
        if (!e.posed) {
            // First play frame that saw it: remember where it stood. A node has
            // to be seen standing still before it can be seen moving.
            e.playPos = p; e.playRot = r; e.playScale = sc; e.posed = true;
        } else if (!(p == e.playPos) || !(r == e.playRot) || !(sc == e.playScale)) {
            node->_setSoftMovable(true);
            movable = true;
            why = iris::MobilityReason::Play;
            e.playPos = p; e.playRot = r; e.playScale = sc;
            if (!e.mobilityWarned) {
                e.mobilityWarned = true;
                ++mMobilityMisses;
                mLastMobilityMiss = node->getName();
                // PLAIN WORDS, once, naming the thing: the author is not a
                // programmer and the fix is one combo box away.
                qWarning("Jahshaka: '%s' started moving during play but is not marked Movable. "
                         "It moves smoothly, but it leaves its old bounce light behind until you "
                         "stop play — set Movement to Movable in its properties to remove that.",
                         qUtf8Printable(node->getName()));
            }
        }
    }

    // THE MOVABLE COUNT AND THE MOVABLE-LIGHT LIST are INCREMENTAL (§3.7): a
    // still frame visits nothing, so neither can be re-folded by a walk. The
    // entry carries its own contribution and gives it back at release.
    if (e.countedMovable != movable) {
        e.countedMovable = movable;
        if (movable) {
            ++mMovableNodes;
            // The GI light signature needs this answer per light and cannot
            // derive it (rule 2 is a question about the whole ancestor chain);
            // applyEnvironment reads the list later in the same frame.
            if (node->getSceneNodeType() == iris::SceneNodeType::Light)
                mMovableLights.push_back(node);
        } else {
            if (mMovableNodes) --mMovableNodes;
            if (node->getSceneNodeType() == iris::SceneNodeType::Light) {
                auto it = std::find(mMovableLights.begin(), mMovableLights.end(), node);
                if (it != mMovableLights.end()) mMovableLights.erase(it);
            }
        }
    }
    // ON CHANGE ONLY, like every other flag on this walk.
    const int want = movable ? 1 : 0;
    // ...and a node the walk has never seen is not a CHANGE: -1 is "never
    // pushed", and its first push is the classification a node is born with.
    // The distinction matters for the settle gate below — an arrival already
    // has its own machinery (the engine's "items appeared" probe stale and the
    // geometry signature the new item joins), and treating it as a flip would
    // adopt a signature read before the scene graph has its transforms, which
    // is the trap applyEnvironment's own "adopt AFTER the push" note describes.
    const bool known = e.movable >= 0;
    if (e.movable != want) {
        // WHY the intent travels with the value: an AUTHORING change moves the
        // object in or out of the voxel bounce, which costs one from-scratch GI
        // rebuild; the play-time soft promotion must cost nothing at all (owner
        // decision O3), so it asks the renderer for the channel change without
        // the GI edge and accepts the stale bounce ghost until play stops.
        //
        // ...and the play STOP that clears such a promotion is soft for the
        // same reason, from the other side: the object never left the voxels,
        // so nothing has to be rebuilt to put it back.
        const bool soft = why == iris::MobilityReason::Play || (!movable && e.movableSoft);
        mTarget->setNodeMovable(e.node, movable,
                                soft ? jahshaka::engine::MobilityChange::Soft
                                     : jahshaka::engine::MobilityChange::Authoring);
        notePush(node, "mobility");
        e.movable = want;
        e.movableSoft = movable && why == iris::MobilityReason::Play;
        if (known) mMobilityChanged = true;
    }
    return movable;
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
    // Particle colours are colours a user picked (§4, pick 3). ALPHA is not a
    // colour — it is the coverage the blend uses — and stays where it was.
    {
        const iris::LinearColor cs = iris::linearOf(ps->emitColourStart);
        const iris::LinearColor ce = iris::linearOf(ps->emitColourEnd);
        e.colourStart = Colour(cs.r, cs.g, cs.b, float(ps->emitColourStart.alphaF()));
        e.colourEnd   = Colour(ce.r, ce.g, ce.b, float(ps->emitColourEnd.alphaF()));
    }
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
        notePush(ps, "particles");
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
    // The ground's horizon holds the FLOOR's material by id and is not an entry,
    // so the sweep cannot see it. A floor deleted in the same frame would
    // otherwise destroy a datablock the horizon's Item still points at — the
    // stale-binding crash, reached from the one object in the scene nobody can
    // select. Its own mesh is not in mMeshes at all (like the grid's).
    if (mHorizonMaterial) usedMaterials.insert(mHorizonMaterial);
    for (auto it = mMeshes.begin(); it != mMeshes.end();) {
        if (usedMeshes.contains(it.value())) { ++it; continue; }
        mTarget->destroyMesh(it.value()); it = mMeshes.erase(it);
    }
    // ...and the per-material MEMO with them. It used to be swept at the end of
    // every sync — one pass over every material in the scene, every frame, and
    // a still frame (which runs no walk, so stamps nothing) would have thrown
    // the whole memo away once a second. It is dropped WITH the material here
    // instead, which is the only moment a key can go stale.
    for (auto it = mMaterials.begin(); it != mMaterials.end();) {
        if (usedMaterials.contains(it.value())) { ++it; continue; }
        // The two per-material records go WITH the material (lead review F7).
        // The memo is keyed by the document material and the item-rebuild
        // serial by the engine one, and this is the one place both die: a
        // serial whose material is gone can never be read again, and engine
        // ids only ever increment so a later material cannot inherit it.
        mMaterialItemSerial.remove(it.value());
        mMaterialSync.remove(it.key());
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
    // A memo entry whose material never reached mMaterials (a conversion that
    // failed) has no other moment to die: sweep the memo against the cache.
    for (auto it = mMaterialSync.begin(); it != mMaterialSync.end();) {
        if (mMaterials.contains(it.key())) { ++it; continue; }
        mMaterialUsers.remove(it.key());
        it = mMaterialSync.erase(it);
    }
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
    // THE AGGREGATES ARE INCREMENTAL (DIRTY_SET_MIRROR_SPEC §3.7): an entry
    // that goes has to give its contribution back, because nothing re-folds
    // the scene any more.
    noteRefractive(e, false, false);
    e.gpuSkinned = false;
    e.characterHost = nullptr;
    noteRigCounts(e);
    if (e.countedMovable) {
        e.countedMovable = false;
        if (mMovableNodes) --mMovableNodes;
        auto it = std::find(mMovableLights.begin(), mMovableLights.end(), e.docNode);
        if (it != mMovableLights.end()) mMovableLights.erase(it);
    }
    if (e.docNode && e.materialPtr) noteMaterialUser(e.docNode, e.materialPtr, nullptr);
    if (mHorizonFloor && static_cast<const iris::SceneNode *>(mHorizonFloor) == e.docNode)
        mHorizonFloor = nullptr;
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
    // THE CACHE FIRST (MIRROR_SCALE lane). This built a whole PbrParams —
    // two dynamic_casts, a std::string for the BRDF name and ten QStrings for
    // the address rows — BEFORE looking, so every re-attach of an already
    // mirrored material paid the full conversion to throw it away. On a scene
    // open that is once per mesh node.
    if (material) {
        auto hit = mMaterials.constFind(material);
        if (hit != mMaterials.constEnd()) {
            // (The noteRefractive call that stood here is GONE: the counts
            // are per ENTRY now, maintained by visitNode where the entry is —
            // a cache HIT here told the old scene-wide flag about a material
            // that may not be attached to anything this frame.)
            return hit.value();
        }
    }
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
/// zero. Distortion "Auto" (POST_LOOKS_SPEC §5.3) is resolved the same way and
/// for the same reason.
///
/// PER ENTRY AND INCREMENTAL (DIRTY_SET_MIRROR_SPEC §3.7). It used to be a
/// boolean the walk re-raised every frame and sync() reset — which a still
/// frame, running no walk, would have reset to false and left there, turning
/// the refraction pass off under a glass object nobody touched. The entry owns
/// its contribution and gives it back when it is released.
void SceneMirror::noteRefractive(Entry &e, bool refractive, bool distortion)
{
    if (e.countedRefractive != refractive) {
        e.countedRefractive = refractive;
        if (refractive) ++mRefractiveEntries;
        else if (mRefractiveEntries) --mRefractiveEntries;
    }
    if (e.countedDistortion != distortion) {
        e.countedDistortion = distortion;
        if (distortion) ++mDistortionEntries;
        else if (mDistortionEntries) --mDistortionEntries;
    }
    mAnyRefractive = mRefractiveEntries > 0;
    mAnyDistortion = mDistortionEntries > 0;
}

/// How many entries carry a GPU rig, and how many of those are pieces of a
/// multi-piece character — the two numbers syncSkeletonSharing and syncClips
/// early-out on. Incremental for the same reason as the counts above.
void SceneMirror::noteRigCounts(Entry &e)
{
    const bool piece = e.gpuSkinned && e.characterHost != nullptr;
    if (e.countedSkinned != e.gpuSkinned) {
        e.countedSkinned = e.gpuSkinned;
        if (e.gpuSkinned) ++mSkinnedNodes;
        else if (mSkinnedNodes) --mSkinnedNodes;
    }
    if (e.countedPiece != piece) {
        e.countedPiece = piece;
        if (piece) ++mCharacterPieces;
        else if (mCharacterPieces) --mCharacterPieces;
    }
}

/// Which document nodes draw each material (§3.6). A MATERIAL edit moves no
/// node, so the change list cannot see one — this index is how the edit reaches
/// the nodes that have to re-push it, without a walk of the scene.
void SceneMirror::noteMaterialUser(iris::SceneNode *node, iris::Material *from,
                                   iris::Material *to)
{
    if (from == to) return;
    if (from) {
        auto it = mMaterialUsers.find(from);
        if (it != mMaterialUsers.end()) {
            auto &v = it.value();
            auto at = std::find(v.begin(), v.end(), node);
            if (at != v.end()) v.erase(at);
            if (v.empty()) {
                mMaterialUsers.erase(it);
                // ...AND THE MEMO WITH IT (lead review R2 #1). Its key is a raw
                // `iris::Material *` and markChangedMaterials DEREFERENCES
                // every key it holds (to read the material's revision), while
                // the memo was pruned only by reclaimUnused — which runs AFTER
                // the change list is consumed. A material dropped by its last
                // node is free to die the moment the caller's reference goes,
                // and that is the ordinary rebake/replace path (materialsapi
                // builds a fresh material, assigns it, and lets the old one go
                // at scope end). Nothing else can reference a material no entry
                // draws, so this is the exact moment its record must go.
                mMaterialSync.remove(from);
            }
        }
    }
    if (to) {
        auto &v = mMaterialUsers[to];
        if (std::find(v.begin(), v.end(), node) == v.end()) v.push_back(node);
    }
}

/// ONE ENGINE PUSH. Counted always; ATTRIBUTED when the verifier made it,
/// because a push the verifier makes is by definition a change the document
/// failed to report and the fix is a missing mark, named.
void SceneMirror::notePush(const iris::SceneNode *node, const char *what)
{
    ++mVisitPushes;
    if (!mVerifying) return;
    ++mVerifierCatches;
    // Once per node+latch pair per process: a miss that repeats every ~2 s for
    // an hour must not fill the log with the same line.
    static QSet<QString> reported;
    const QString key = QStringLiteral("%1/%2")
                            .arg(node ? node->name : QStringLiteral("<none>"))
                            .arg(QLatin1String(what));
    if (reported.contains(key)) return;
    reported.insert(key);
    qWarning("SceneMirror: the change list MISSED '%s' (%s) — the document changed it without "
             "reporting it (SceneNode::notifyChanged / Material::touch). The verifier pushed it; "
             "the fix is the missing mark, not this message.",
             qUtf8Printable(node ? node->name : QStringLiteral("<unnamed>")), what);
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
/// EVERY DOCUMENT FIELD `materialSyncFor` AND `toPbrParams` READ, as one hash.
///
/// The memo's validity key (see MaterialSync in the header). All of it is PODs,
/// QColors (four ints) and the material's own texture map — no allocation, no
/// dynamic_cast (the caller passes the resolved one), no string construction
/// except the texture map's own keys, which a material without maps does not
/// have at all.
///
/// A FIELD ADDED TO PbrMaterial AND FORGOTTEN HERE is an edit that never
/// reaches the renderer, which is why mirror.scale drives every one of the
/// material's own authored property rows through setValue and asserts that each
/// moves this number.
namespace {
/// THE FINGERPRINT'S HASHER. Hasher above mixes one BYTE at a time, which is
/// right for strings and four times the work for a wall of floats and ints —
/// the same reason mixFloat exists a few lines below it. This one mixes a WORD
/// per field, and it runs over ~40 fields per material per sync.
struct FieldHasher {
    quint64 h = 1469598103934665603ull;
    inline FieldHasher &operator<<(quint32 v) { h ^= v; h *= 1099511628211ull; return *this; }
    inline FieldHasher &operator<<(int v)     { return *this << quint32(v); }
    inline FieldHasher &operator<<(bool v)    { return *this << quint32(v ? 1 : 0); }
    inline FieldHasher &operator<<(float f)
    { quint32 b; std::memcpy(&b, &f, sizeof b); return *this << b; }
    /// AT THE PRECISION THE CONVERSION READS. toPbrParams takes redF()/greenF()
    /// /blueF(), which come off QColor's 16-BIT storage; rgba() is the 8-bit
    /// view, so hashing that would let a sub-1/255 edit slip past the memo.
    /// Nothing in the tree writes a colour that fine today — the panel's picker
    /// and every reader are 8-bit — which is exactly why it would have been a
    /// silent trap rather than a visible one (lead review F5).
    inline FieldHasher &operator<<(const QColor &c)
    {
        const QRgba64 q = c.rgba64();
        return *this << quint32(q.red() << 16 | q.green())
                     << quint32(q.blue() << 16 | q.alpha());
    }
    FieldHasher &operator<<(const QString &s)
    {
        *this << quint32(s.size());
        const char16_t *d = reinterpret_cast<const char16_t *>(s.utf16());
        for (qsizetype i = 0; i < s.size(); ++i) *this << quint32(d[i]);
        return *this;
    }
};
}  // namespace

quint64 SceneMirror::materialFingerprint(iris::Material *material, iris::PbrMaterial *pbr)
{
    FieldHasher h;
    if (!material) return h.h;
    // The texture MAP, first and for both material classes: the slot table
    // below reads `textures`, and a map bound or cleared has to move the hash.
    h << quint32(material->textures.size());
    for (auto t = material->textures.constBegin(); t != material->textures.constEnd(); ++t) {
        h << t.key();
        h << (t.value() ? t.value()->source : QString());
    }
    if (pbr) {
        h << quint32(1);
        h << pbr->baseColor << pbr->baseColorFactor
          << pbr->metallicFactor << pbr->roughnessFactor
          << pbr->roughnessLowerBound << pbr->roughnessUpperBound
          << pbr->emissiveColor << pbr->emissiveIntensity
          << pbr->alphaMode << pbr->refractionStrength << pbr->alpha << pbr->alphaCutoff
          << pbr->normalFactor
          << pbr->textureScale << pbr->textureScaleV
          << pbr->textureOffsetU << pbr->textureOffsetV << pbr->textureRotation
          << int(pbr->renderStates.rasterState.cullMode)
          << pbr->shadingModel << pbr->brdf
          << pbr->clearCoat << pbr->clearCoatRoughness
          << pbr->receiveShadows << pbr->emissiveAsLightmap
          << pbr->workflow
          << pbr->specularColor << pbr->ior
          << pbr->fresnelColor
          << pbr->useFresnelColor << pbr->separateFresnel
          << pbr->anisotropy;
        // Per-map addressing. The hash is EMPTY on an unauthored material (the
        // absent-means-Wrap rule), so the overwhelming majority of materials
        // pay nothing for this loop.
        h << quint32(pbr->mapAddress.size());
        for (auto a = pbr->mapAddress.constBegin(); a != pbr->mapAddress.constEnd(); ++a)
            h << a.key() << a.value();
        for (int i = 0; i < iris::PbrMaterial::kDetailLayers; ++i) {
            const auto &d = pbr->detail[i];
            h << d.blend << d.offsetU << d.offsetV << d.scaleU << d.scaleV
              << d.weight << d.normalWeight;
        }
        return h.h;
    }
    if (auto *def = dynamic_cast<iris::DefaultMaterial *>(material)) {
        h << quint32(2) << def->getDiffuseColor()
          << def->getShininess() << def->getTextureScale();
        return h.h;
    }
    h << quint32(0);
    return h.h;
}

const SceneMirror::MaterialSync &SceneMirror::materialSyncFor(iris::Material *material)
{
    // THE MATERIAL'S OWN REVISION IS THE FAST PATH (DIRTY_SET_MIRROR_SPEC
    // §3.6); the FINGERPRINT stays as the ORACLE, computed when the revision
    // moved and on every verification visit. A counter is only as good as the
    // writer that bumps it — this is what makes a writer that forgot a
    // COUNTED, self-healing event instead of a silent one.
    const quint32 rev = material ? material->revision() : 0;
    auto it = mMaterialSync.find(material);
    if (it != mMaterialSync.end()) {
        // Already validated by THIS walk: however many nodes share the
        // material, it is fingerprinted once.
        if (it->lastSeen == mSyncStamp) return it.value();
        it->lastSeen = mSyncStamp;
        // THE VERIFIER'S MATERIAL QUOTA. Re-hashing forty fields and a texture
        // map is ~15x the cost of re-checking a node's latches, so the two
        // budgets are separate: the node pass runs 64 a sync and this runs 8,
        // and full coverage of a material per primitive takes seconds rather
        // than milliseconds. The DIFFERENTIAL suite is what catches a miss at
        // once; this is the always-on background net.
        const bool forced = mVerifying && mVerifyMaterialQuota > 0;
        if (!forced && it->revision == rev) return it.value();
        if (forced) --mVerifyMaterialQuota;
        const quint64 fp = materialFingerprint(material, it->asPbr);
        if (fp == it->fingerprint) {          // nothing the build reads moved
            it->revision = rev;
            return it.value();
        }
        // The fields moved. If the REVISION did not, the write bypassed
        // Material::touch() — count it, name it, and heal it.
        if (it->revision == rev) {
            // NAMED (lead review R2 #4): a material has a name and a guid, and
            // "something was written without touch()" is useless without them.
            const QByteArray reason =
                QStringLiteral("material '%1' written without touch()")
                    .arg(material->getName().isEmpty() ? material->getGuid()
                                                       : material->getName())
                    .toUtf8();
            notePush(nullptr, reason.constData());
        }
        // Fall through and rebuild in place, keeping the cast.
        MaterialSync fresh;
        fresh.lastSeen = mSyncStamp;
        fresh.asPbr = it->asPbr;
        fresh.revision = rev;
        it.value() = fresh;
    } else {
        MaterialSync fresh;
        fresh.lastSeen = mSyncStamp;
        fresh.asPbr = dynamic_cast<iris::PbrMaterial *>(material);
        fresh.revision = rev;
        it = mMaterialSync.insert(material, fresh);
    }
    MaterialSync &ms = it.value();
    ++mMaterialBuilds;
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
    if (ms.asPbr) sharedSrgb = iris::PbrMaterial::sharedMapIsSrgb(ms.asPbr->workflow);

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
    // LAST, so a throw or an early return above cannot leave a fingerprint
    // standing over a half-built description.
    ms.fingerprint = materialFingerprint(material, ms.asPbr);
    return ms;
}

void SceneMirror::syncTextures(Entry &e, const MaterialSync &ms)
{
    // (It took the document material and re-probed the memo for it — a second
    // QHash lookup per mesh node per frame to fetch what the caller already had
    // in hand. MIRROR_SCALE lane.)
    if (!e.material || e.material == mDefaultMaterial) return;
    const std::vector<TextureBind> &binds = ms.binds;
    const quint64 signature = ms.textureSignature;
    if (e.texturesPushed && signature == e.textureSignature) return;
    notePush(e.docNode, "textures");
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
        // THE COLOUR-SPACE RULE (SKY_LIGHT_SPEC.md §4, owner decision §188e):
        // an 8-bit colour a user picked is sRGB and enters the renderer LINEAR,
        // exactly like the 8-bit albedo TEXTURE the sampler decodes. Before
        // this a "50% grey" material rendered 2.33x brighter than a 50% grey
        // texture of the same colour. `iris::linearOf` is the one helper and
        // every colour a user picks goes through it.
        const iris::LinearColor c = iris::linearOf(pbr->baseColor);
        const float f = pbr->baseColorFactor;
        out.albedo    = Colour(c.r * f, c.g * f, c.b * f, 1.0f);
        out.metalness = pbr->metallicFactor;
        // The document's roughness remap bounds apply per-texel to a sampled map;
        // the engine has no such remap, so approximate by clamping the scalar
        // factor into the (order-normalised) bounds.
        const float lo = std::min(pbr->roughnessLowerBound, pbr->roughnessUpperBound);
        const float hi = std::max(pbr->roughnessLowerBound, pbr->roughnessUpperBound);
        out.roughness = std::max(lo, std::min(pbr->roughnessFactor, hi));
        const iris::LinearColor e = iris::linearOf(pbr->emissiveColor);
        out.emissive  = Colour(e.r * pbr->emissiveIntensity, e.g * pbr->emissiveIntensity,
                               e.b * pbr->emissiveIntensity, 1.0f);
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
        const iris::LinearColor sc = iris::linearOf(pbr->specularColor);
        out.specularColour = Colour(sc.r, sc.g, sc.b, 1.0f);
        // `ior` is a SCALAR, not a colour: it is a refractive index, it was
        // never sRGB-encoded and it does not pass through linearOf.
        out.ior = pbr->ior;
        const iris::LinearColor fc = iris::linearOf(pbr->fresnelColor);
        out.fresnelColour = Colour(fc.r, fc.g, fc.b, 1.0f);
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
        const iris::LinearColor c = iris::linearOf(def->getDiffuseColor());
        out.albedo    = Colour(c.r, c.g, c.b, 1.0f);
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
    return toLightDesc(light, nullptr, false);
}

Colour SceneMirror::atmosphereTintFor(const iris::LightNode *light,
                                      const iris::LightNode *sun) const
{
    const Colour white(1.0f, 1.0f, 1.0f, 1.0f);
    if (!mTarget || !light || light->lightType != iris::LightType::Directional) return white;
    if (!light->followsAtmosphere) return white;
    // THE SUN ONLY. A secondary directional is not the sun (it draws no disc
    // and casts no shadow), and tinting it would be a second, invisible sun
    // following the sky.
    const iris::LightNode *resolved = sun;
    if (!resolved) {
        const auto scene = const_cast<iris::LightNode *>(light)->getScene();
        const auto own = scene ? scene->sunLight() : iris::LightNodePtr();
        resolved = own.data();
    }
    if (resolved != light) return white;
    const iris::Vec3 travel = const_cast<iris::LightNode *>(light)->getLightDir();
    if (travel.lengthSquared() < 1e-12f) return white;
    const iris::Vec3 toSun = -travel.normalized();
    // The ENGINE decides whether there is an atmosphere to ask at all: it
    // answers white for every other sky, so "is the realistic sky on?" is not
    // a second opinion kept here.
    return mTarget->atmosphereSunTint(Vec3(toSun.x(), toSun.y(), toSun.z()));
}

LightDesc SceneMirror::toLightDesc(iris::LightNode *light, iris::LightNode *sun, bool sunKnown,
                                   const Colour &sunTint)
{
    LightDesc d;
    switch (light->lightType) {
    case iris::LightType::Directional: d.type = LightType::Directional; break;
    case iris::LightType::Spot:        d.type = LightType::Spot; break;
    case iris::LightType::Area:        d.type = LightType::Area; break;
    case iris::LightType::Point: default: d.type = LightType::Point; break;
    }
    // A LIGHT'S COLOUR IS A COLOUR THE USER PICKED (§4, pick 3): decoded, like
    // every other one. White is 1.0 either way; a tinted light's saturation
    // moves, which is the point — the picker means one thing everywhere now.
    //
    // ...TIMES WHAT THE AIR DOES TO IT (SUN_FOLLOWS_ATMOSPHERE, lane ENGINE-7
    // item 6). White for every light but a sun that follows the atmosphere, so
    // this line is the picked colour itself in every other scene; on the sun it
    // makes the picked colour the NOON colour and lets a low sun arrive red and
    // dim. The sun DISC is multiplied by the same value at the same moment
    // (applySky), so the two cannot disagree.
    {
        const iris::LinearColor lc = iris::linearOf(light->color);
        d.colour = Colour(lc.r * sunTint.r, lc.g * sunTint.g, lc.b * sunTint.b, 1.0f);
    }
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
    // ---- THE SUN, and the secondary directionals -----------------------
    // Our shadow node declares exactly ONE directional slot (three PSSM splits
    // at slot 0; every focused slot accepts spot/point only — OgreShadow.cpp).
    // With two shadow-casting directionals Ogre filled that slot by its own
    // castShadows-then-light-id sort, i.e. by engine creation order, which can
    // flip across a reload: one of the two suns cast, silently, and which one
    // was luck. The document's ONE resolver decides instead, and a directional
    // that is not the sun is pushed as a non-caster — Ogre's sort then has a
    // single candidate and the answer is the same on every frame and reload.
    // (This is Unreal's Forward Shading Priority semantic exactly: one main
    // directional casts. A SECOND PSSM set is out of scope — it is three more
    // full-view-frustum scene passes EVERY frame, the most expensive shadow we
    // render.)
    if (light->lightType == iris::LightType::Directional) {
        d.forwardShadingPriority = light->forwardShadingPriority;
        // Through the light's OWN scene, which is what makes this a pure
        // function of the node (the mirror's `mSource` is that same scene, and
        // this is called from a static context too). A light that is not in a
        // scene yet is the only directional there is, so it is the sun.
        // RESOLVED BY THE CALLER when it has a whole light list to push (the
        // per-sync walk): the answer is the same for every light in one sync,
        // and computing it here costs a QVector and a sort per directional.
        iris::LightNode *resolved = sun;
        if (!sunKnown) {
            const auto scene = light->getScene();
            const auto own = scene ? scene->sunLight() : iris::LightNodePtr();
            resolved = own.data();
        }
        d.primaryDirectional = !resolved || resolved == light;
        if (!d.primaryDirectional) d.castShadows = false;
        // ...AND A SUN THAT HAS SET CASTS NOTHING (round-2 review item 4): its
        // three PSSM passes would render every frame for a light whose colour
        // the atmosphere has taken to zero. `sunTint` is white for every light
        // that does not follow the atmosphere, so this can only fire on a sun
        // that does.
        if (sunTintIsNight(sunTint)) d.castShadows = false;
    }
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
        if (e.hasDecal) {
            mTarget->removeDecal(e.node);
            notePush(decal, "decal removed");
            e.hasDecal = false;
        }
        e.decalSignature = sig;
        return;
    }

    DecalDesc d = toDecalDesc(decal);
    d.diffuse = diffuse;
    d.normal = decalTextureFor(decal->resolvedNormalPath, DecalMap::Normal);
    d.emissive = decalTextureFor(decal->resolvedEmissivePath, DecalMap::Emissive);
    // ON CHANGE ONLY, like every other push in this walk (DIRTY_SET_MIRROR_SPEC
    // §4). setDecal rebinds three atlas slices and rewrites the projector's
    // parameters, and it ran EVERY frame for every decal in the scene — the
    // last unlatched per-frame push left in the walk, and one the oracle
    // (verifyAgainstFullWalk) would otherwise report as a miss forever.
    Hasher dk;
    dk << quint64(d.diffuse) << quint64(d.normal) << quint64(d.emissive)
       << d.width << d.height << d.depth << d.metalness << d.roughness
       << quint32(d.ignoreAlphaDiffuse ? 1 : 0);
    if (!e.hasDecal || e.decalPushKey != dk.h) {
        if (mTarget->setDecal(e.node, d)) {
            notePush(decal, "decal");
            e.hasDecal = true;
            e.decalPushKey = dk.h;
            if (rebind) e.decalSignature = sig;
        }
    } else if (rebind) {
        e.decalSignature = sig;
    }
}

// The wire box: 12 edges of the projector volume plus a tick down -Y showing
// which way it projects. Always on with the helpers toggle (like the area
// light's rectangle, it IS the object's shape, not a falloff volume).
void SceneMirror::syncDecalWires(Entry &e, iris::DecalNode *decal)
{
    if (!mLightWires) {
        if (e.wireNode && e.wireVisible != 0) {
            mTarget->setNodeVisible(e.wireNode, false);
            e.wireVisible = 0;
        }
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
    const Vec3 wireScale(std::max(decal->width, 0.001f) * (s.x() > 1e-6f ? 1.0f / s.x() : 1.0f),
                         std::max(decal->depth, 0.001f) * (s.y() > 1e-6f ? 1.0f / s.y() : 1.0f),
                         std::max(decal->height, 0.001f) * (s.z() > 1e-6f ? 1.0f / s.z() : 1.0f));
    // ON CHANGE ONLY, exactly like the light wires' (which have had this latch
    // since MIRROR_SCALE): setNodeTransform is a real engine write plus a node
    // dirty, and this ran per decal per frame for a box whose size is three
    // hand-edited numbers.
    Hasher wireKey;
    wireKey << wireScale.x << wireScale.y << wireScale.z;
    if (!e.wireXformPushed || e.wireXformKey != wireKey.h) {
        mTarget->setNodeTransform(e.wireNode, Vec3(), Quat(), wireScale);
        e.wireXformKey = wireKey.h;
        e.wireXformPushed = true;
        notePush(decal, "decal wire");
    }
    if (e.wireVisible != 1) { mTarget->setNodeVisible(e.wireNode, true); e.wireVisible = 1; }
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
            const auto it = skeleton->boneMap.constFind(b->parent()->name);
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
            for (iris::Bone *up = b->parent().data(); up; up = up->parent().data()) {
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
    QVector<iris::SceneNode *> pieceNodes;
    stack.append(const_cast<iris::SceneNode *>(host));
    for (int i = 0; i < stack.size(); ++i) {
        iris::SceneNode *n = stack[i];
        if (n->getSceneNodeType() == iris::SceneNodeType::Mesh) {
            auto *mn = static_cast<iris::MeshNode *>(n);
            if (!mn->skeleton.isNull()) {
                skels.append(mn->skeleton); hashPtr(mn->skeleton.data());
                pieceNodes.append(n);
            }
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
        markPiecesDirty(pieceNodes);
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
    // ...AND EVERY OTHER PIECE OWES A RE-ATTACH when it moved (§3.6's shape,
    // for rigs): a piece joining or leaving changes the bone list all of them
    // are skinned against, and nothing WROTE those other nodes. The walk found
    // it by visiting them; the change list has to be told.
    if (fresh.epoch != rec.epoch) markPiecesDirty(pieceNodes);
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
    // A HASH, not a concatenated string (MIRROR_SCALE lane): this ran per
    // skinned node per frame, and a Mixamo character with 30 clips built ~100
    // QStrings to produce a value whose only use is a compare with last
    // frame's.
    Hasher sig;
    sig << quint32(e.rigId.size());
    sig.bytes(e.rigId.data(), e.rigId.size());
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
            sig << anim->getName() << anim->getLength() << quintptr(anim.data());
        }
    }
    const quint64 signature = sig.h;
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
            const auto pit = e.rigSkeleton->boneMap.constFind(bones[int(i)]->parent()->name);
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

/// The 16 floats of a world transform, hashed — the fallback push's change
/// guard (lane ENGINE-7 item 4). Raw bytes, like every other change key in this
/// file: it answers "is this the same matrix as last sync?", never "how far did
/// it move?".
quint64 worldKeyOf(const iris::Mat4 &m)
{
    quint64 h = 1469598103934665603ull;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            quint32 bits;
            const float f = m(r, c);
            std::memcpy(&bits, &f, sizeof(bits));
            for (int i = 0; i < 4; ++i) { h ^= (bits & 0xFF); h *= 1099511628211ull; bits >>= 8; }
        }
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
                // ONLY IF IT IS ON A TAG. releaseRider used to run here every
                // sync ("it may have been on a tag a moment ago") and it is not
                // free: it writes the rider's world transform back and REMOVES
                // its row — so on the fallback path the row was rebuilt every
                // frame, and with it the "authored local" snapshot below, whose
                // whole job is to survive the spell on the fallback (D4). Taken
                // from the current local each frame, that snapshot recorded the
                // WORLD transform the previous frame had written into it.
                // WHAT THE RIDER'S LOCAL MEANS RIGHT NOW, read BEFORE anything
                // bakes a world into it (round-2 review, item 3). While a rider
                // is on a tag its local IS its socket offset (D4) — and
                // releaseRider writes the tag's WORLD into that same field,
                // which is the pose-keeping promise for a rider that is being
                // let go for good. Snapshotting `authored*` AFTER the release
                // therefore recorded a world transform as the offset, and the
                // re-arm when the rig came back restored THAT: after any spell
                // without a rig — a model swap, a mesh reload, a re-import —
                // the prop rode off the bone by its own world position.
                iris::Vec3 keepPos = rider->getLocalPos(), keepScale = rider->getLocalScale();
                iris::Quat keepRot = rider->getLocalRot();
                {
                    const auto prior = mBoneRiders.constFind(rider);
                    // THE ROW KNOWS BETTER THAN THE NODE. While the rider was on
                    // its tag we recorded the offset every sync, precisely
                    // because it is gone by the time we get here: the engine
                    // frees every rider on a skeleton it rebuilds, and Ogre's
                    // detach re-expresses the node's local as it does so
                    // (measured: it reads zero on the frame a model swap takes
                    // the rig away). Only a rider we have never seen armed
                    // falls back to reading the node.
                    if (prior != mBoneRiders.constEnd() &&
                        (prior->fallbackDriven || prior->authoredValid)) {
                        keepPos = prior->authoredPos;
                        keepRot = prior->authoredRot;
                        keepScale = prior->authoredScale;
                    }
                }
                if (iris::graph::isSocketRider(rider->graphNode())) releaseRider(rider);
                // ...AND STRAIGHT BACK INTO THE ROW. releaseRider drops it, and
                // the resolve below can fail (a socket whose bone the rig has
                // not, a frame where the owner has no pose yet) — which used to
                // `continue` past the snapshot and lose the offset for good.
                // The authored offset is the one thing about a rider that must
                // outlive every one of those.
                {
                    RiderState &kept = mBoneRiders[rider];
                    if (!kept.authoredValid) {
                        kept.authoredPos = keepPos;
                        kept.authoredRot = keepRot;
                        kept.authoredScale = keepScale;
                        kept.authoredValid = true;
                    }
                }
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
                st.fallbackDriven = true;    // the authored offset is already kept, above
                // CHANGE-GUARDED (lane ENGINE-7 item 4). This is a DOCUMENT
                // write through the marking setters, so a still scene holding a
                // socketed prop on an unrigged owner bumped the transform-write
                // epoch on every frame and re-ran every O(scene) walk hanging
                // off it (nodegraph.h). It happens when the socket's world
                // really moved — or when something else wrote the rider's local
                // since we did, because while a rider is attached the socket
                // owns its placement and an outside write must still be
                // overwritten, exactly as the unguarded push did.
                const quint64 key = worldKeyOf(world);
                const auto differs = [](float a, float b) {
                    return std::fabs(double(a) - double(b)) > 1e-5;
                };
                const iris::Vec3 lp = rider->getLocalPos(), ls = rider->getLocalScale();
                const iris::Quat lr = rider->getLocalRot();
                const bool drifted =
                    differs(lp.x(), st.lastLocalPos.x()) || differs(lp.y(), st.lastLocalPos.y()) ||
                    differs(lp.z(), st.lastLocalPos.z()) ||
                    differs(ls.x(), st.lastLocalScale.x()) || differs(ls.y(), st.lastLocalScale.y()) ||
                    differs(ls.z(), st.lastLocalScale.z()) ||
                    differs(lr.x(), st.lastLocalRot.x()) || differs(lr.y(), st.lastLocalRot.y()) ||
                    differs(lr.z(), st.lastLocalRot.z()) || differs(lr.scalar(), st.lastLocalRot.scalar());
                if (!st.fallbackWorldPushed || st.fallbackWorldKey != key || drifted) {
                    rider->setGlobalTransform(world);
                    rider->update(0.0f);
                    st.fallbackWorldKey = key;
                    st.fallbackWorldPushed = true;
                    st.lastLocalPos = rider->getLocalPos();
                    st.lastLocalRot = rider->getLocalRot();
                    st.lastLocalScale = rider->getLocalScale();
                    notePush(rider, "socket fallback");
                }
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
                    // THE ROW SURVIVES A FAILED ARM (round-2 review, item 3).
                    // It used to be removed — and with it the AUTHORED OFFSET
                    // this rider must get back when the rig returns. A failed
                    // arm is exactly the moment that matters: the owner's model
                    // was swapped, so its rig is gone for a few frames, and the
                    // engine's own free has already re-expressed the rider's
                    // local (measured: it reads zero). What must not survive is
                    // the claim to be ATTACHED, so that is what is cleared.
                    state.owner = 0;
                    state.bone.clear();
                    state.offsetKey = 0;
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
            // ...AND THE OFFSET IN FORCE, which is the same reading while the
            // rider is ON its tag (D4) and is the only place it can be taken
            // from: a spell without a rig starts with the engine freeing the
            // tag, which re-expresses this very field (round-2 review, item 3).
            state.authoredPos = state.lastLocalPos;
            state.authoredRot = state.lastLocalRot;
            state.authoredScale = state.lastLocalScale;
            state.authoredValid = true;
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
    // BELOW TWO CHARACTER PIECES THERE IS NOTHING TO SHARE, and this used to
    // find that out by iterating EVERY entry in the scene, every frame
    // (MIRROR_SCALE lane): 8,404 QHash probes per frame on a lattice with no
    // skeleton in it at all. The walk counts the pieces as it goes.
    if (mCharacterPieces < 2) { mShareGroups.clear(); return; }

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
            master->clipSignature = 0;          // it owns its clips again
            master->lastClipPush.clear();
        }
        // THE WORLD COMPARISONS, ONLY WHEN A WORLD CAN HAVE MOVED. Eligibility
        // is a rig-id match plus "this piece sits exactly where the master
        // does", and the second half costs a derived-transform resolution on
        // the master AND on every piece — per frame, for an answer that cannot
        // change unless something wrote a transform. The document's global
        // write counter is the gate; a re-attach (which can change `rigId`)
        // clears the memo itself.
        const unsigned long long writes = iris::graph::transformWrites();
        const bool worldsMayHaveMoved = writes != mShareWorldWrites;
        iris::Mat4 masterWorld;
        if (worldsMayHaveMoved) masterWorld = master->docNode->getGlobalTransform();

        for (Entry *e : group) {
            if (e == master) continue;
            if (worldsMayHaveMoved || !e->shareEligibleValid) {
                if (!worldsMayHaveMoved) masterWorld = master->docNode->getGlobalTransform();
                e->shareEligible = e->rigId == master->rigId &&
                                   sameWorld(e->docNode->getGlobalTransform(), masterWorld);
                e->shareEligibleValid = true;
            }
            const bool eligible = e->shareEligible;
            const bool shared = mTarget->sharesSkeleton(e->node);
            if (eligible && (!shared || e->shareMaster != master->node)) {
                if (mTarget->shareSkeleton(e->node, master->node)) {
                    e->shareMaster = master->node;
                    // A follower holds NO clips (the engine drops them when the
                    // instance goes): forget what we think it has, so that if it
                    // ever un-shares the clip pass re-attaches from scratch.
                    e->clipSignature = 0;
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
                e->clipSignature = 0;           // it needs its own clips again
                e->clipMap.clear();
                e->clipIdMap.clear();
                e->clipNameMap.clear();
                e->lastClipPush.clear();
            } else {
                e->shareMaster = shared ? master->node : 0;
            }
        }
    }
    mShareWorldWrites = iris::graph::transformWrites();
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
    // NO SKINNED NODE, NO PASS (MIRROR_SCALE lane). This walked every entry in
    // the scene every frame to discover that none of them was rigged — 8,403
    // QHash probes a frame on a lattice of cubes, 1.16 ms of the capture. The
    // walk counts the rigged nodes as it goes.
    if (mSkinnedNodes == 0) return;
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
    mFogPushed = false;
    // GI IS NOT RE-PUSHED (ENGINE_CACHE_POLICY_SPEC P10). The GI latch used to
    // be dropped here too, and the re-push that followed is a from-scratch
    // rebuild in the engine — voxels, the whole probe grid, the irradiance
    // field — on every page return: 2.1-2.9 s of blocked UI in the owner's log
    // after "materials -> editor". What a re-take actually needs is the
    // process-wide HlmsPbs binding pointed back at THIS scene's arms, which are
    // still valid; the next applyEnvironment asks the engine for exactly that
    // (Scene::reassertGiBinding) and nothing more.
    mGiReassertPending = true;
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
        // world.refreshShadows(): one re-render of every cached shadow map per
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
    // AMBIENT IS THE SKY LIGHT, AND NOTHING ELSE (SKY_LIGHT_SPEC.md §2, owner
    // decision D14). There is one path and one seam: the sky's own
    // cosine-convolved integral, scaled by the scene's Sky Light — its
    // intensity times its tint, decoded sRGB->linear like every other colour a
    // user picks (§4) — pushed through setAmbientSh.
    //
    // NO SKY LIGHT = 27 ZEROS. Not "the old flat colour", not "a small default":
    // a scene with no Sky Light has no ambient at all, which is the decided
    // behaviour and the reason the two-light default scene goes black when both
    // lights are deleted. The flat Engine::setAmbient path is not gone — it is
    // an ENGINE verb the preview scenes and the engine-side tests still use —
    // but no document path reaches it any more.
    {
        float sh[27] = { 0.0f };
        // THE SKY'S INTEGRAL COMES FROM THE ENGINE (SKY-GPU): it captured the
        // sky it drew into a cubemap and integrated that. Read every frame —
        // 27 floats — because the capture lands one frame after the sky change
        // that asked for it, exactly like the IBL convolution.
        const bool hasSky = mTarget->skyAmbientSh(mSkyAmbientSh);
        const auto skyLight = mSource->skyLight();
        if (skyLight && hasSky) {
            const iris::LinearColor tint = iris::linearOf(skyLight->color);
            const float gain[3] = { tint.r * skyLight->intensity,
                                    tint.g * skyLight->intensity,
                                    tint.b * skyLight->intensity };
            for (int i = 0; i < 9; ++i)
                for (int c = 0; c < 3; ++c) sh[i * 3 + c] = mSkyAmbientSh[i * 3 + c] * gain[c];
        }
        // Push on CHANGE only. The coefficients feed a pass buffer that HlmsPbs
        // rebuilds per pass anyway, but setSphericalHarmonics also re-decides the
        // ambient shader variant, so a per-frame push of an unchanged value was
        // asking a shader/root-layout question every frame for nothing.
        bool changed = !mAmbientPushed;
        for (int i = 0; !changed && i < 27; ++i) changed = sh[i] != mLastAmbientSh[i];
        if (changed) {
            mTarget->setAmbientSh(sh);
            std::memcpy(mLastAmbientSh, sh, sizeof(sh));
            mAmbientPushed = true;
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
        // Decoded like the sky it fades into (§4) — they are the same grey in
        // every sample, and a raw fog against a decoded sky would not match.
        const iris::LinearColor f = iris::linearOf(mSource->fogColor);
        fog.colour = Colour(f.r, f.g, f.b, 1.0f);
        fog.density = mSource->fogDensity;
        fog.heightDensity = mSource->fogHeightDensity;
        fog.heightFalloff = mSource->fogHeightFalloff;
        fog.heightLevel = mSource->fogHeightLevel;
        fog.breakMinBrightness = mSource->fogBreakMinBrightness;
        fog.breakFalloff = mSource->fogBreakFalloff;
        fog.atmosphereColour = mSource->fogAtmosphere;
        const bool changed =
            !mFogPushed || mLastFog.enabled != fog.enabled ||
            mLastFog.colour.r != fog.colour.r || mLastFog.colour.g != fog.colour.g ||
            mLastFog.colour.b != fog.colour.b || mLastFog.density != fog.density ||
            mLastFog.heightDensity != fog.heightDensity ||
            mLastFog.heightFalloff != fog.heightFalloff ||
            mLastFog.heightLevel != fog.heightLevel ||
            mLastFog.atmosphereColour != fog.atmosphereColour ||
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
        // NO BOUNDS TRAVEL ANY MORE (owner decision D8): GiParams::boundsMin ==
        // boundsMax == 0 is the engine's "fit it yourself", and leaving the
        // field at its default is how this mirror says so. `autoBoundsMax`
        // likewise keeps the engine's own default ceiling. The document has no
        // bounds fields to push.
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
        gi.probeCaptureSize = qBound(0, mSource->giProbeCaptureSize, 1024);
        gi.probeHdr = toggle(mSource->giProbeHdr);
        gi.probeShadows = toggle(mSource->giProbeShadows);
        gi.probeOverlap = mSource->giProbeOverlap;
        // PHOTON cascades (SPECS/PHOTON_SPEC.md P0): the switch and, optionally,
        // the table. A row with a non-positive half size or resolution is not a
        // request the renderer can honour halfway, so the whole table is dropped
        // and the tier's own decides — the same rule the engine states.
        gi.cascades = mSource->giCascades;
        gi.cascadeCount = 0;
        for (const iris::Vec3 &row : mSource->giCascadeSet) {
            if (gi.cascadeCount >= 8) break;
            if (row.x() <= 0.0f || row.y() <= 0.0f) { gi.cascadeCount = 0; break; }
            gi.cascadeSet[gi.cascadeCount].halfSize   = row.x();
            gi.cascadeSet[gi.cascadeCount].resolution = int(row.y());
            gi.cascadeSet[gi.cascadeCount].stepCells  = row.z();
            ++gi.cascadeCount;
        }
        gi.probeSnapDeviation = mSource->giProbeSnapDeviation;
        gi.probeSnapSidesMin = mSource->giProbeSnapSidesMin;
        gi.probeSnapSidesMax = mSource->giProbeSnapSidesMax;
        gi.updateBudget = qMax(0, mSource->giUpdateBudget);        // FIX WAVE B1
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
        // ...and, since ENGINE_CACHE_POLICY_SPEC P7, each light's GI PARAMETERS
        // beside its transform (lightGiParamSignature says why).
        //
        // STILL LAMPS ONLY (REALTIME_REFLECTIONS_SPEC §3.3.4, owner decision
        // O2). A lamp the document resolved as MOVING — a carried torch, a
        // swaying pendant, a light on an animated rig — is hashed into its OWN
        // key below and never into this one, because this one arms the settle:
        // a lamp that moves every frame would hold the stability window open
        // for ever and then pay a full re-solve (a teardown, a re-voxelize and
        // every probe re-captured) the moment it paused. What a moving lamp
        // gets instead is the CHEAP path on its own cadence — its light
        // re-injected into the voxels that are already there, about six times a
        // second — which is measurably free and is what makes its bounce follow
        // it. The probes hold the still room's lighting, as designed.
        quint64 lightSig = 0, movableLightSig = 0;
        const auto movingLamp = [&](const iris::LightNode *l) {
            const auto *n = static_cast<const iris::SceneNode *>(l);
            return std::find(mMovableLights.begin(), mMovableLights.end(), n) != mMovableLights.end();
        };
        if (driver) {
            Hasher h;
            h << worldTrsSignature(driver->graphNode()) << lightGiParamSignature(driver);
            // Instant Radiosity traces from ONE light; if that one moves it is
            // the same argument, and the re-trace rides the same cheap cadence.
            if (movingLamp(driver)) movableLightSig = h.h;
            else                    lightSig = h.h;
        } else if (gi.mode == GiMode::Vct || gi.mode == GiMode::VctPccHybrid) {
            mGiChainMemo.clear();          // capacity kept; contents are per call
            Hasher h, m;
            for (const auto &l : mSource->lights) {
                if (l.isNull()) continue;
                Hasher &into = movingLamp(l.data()) ? m : h;
                into << worldTrsSignatureMemo(l->graphNode(), mGiChainMemo)
                     << lightGiParamSignature(l.data());
            }
            lightSig = h.h;
            movableLightSig = m.h;
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
        // THE MATERIAL TERM (ENGINE_CACHE_POLICY_SPEC P7), kept OUT of the
        // signature above on purpose: a material edit arms the same
        // one-re-solve-on-settle debounce but NOT the cheap light re-inject
        // cadence (and its irradiance-field reset) — nothing a re-inject reads
        // changed. Every mode (Instant Radiosity re-traces on it too).
        const quint64 matSig = mTarget->giMaterialSignature();
        // Compared BY VALUE (GiParams::operator==, beside the struct — every
        // field setGlobalIllumination reads is in it, so a new field cannot fall
        // behind the comparison the way a lambda one file away did).
        //
        // A screen re-take (invalidateEnvironment): point the process-wide GI
        // binding back at this scene's arms. Before any push below, which — if
        // the parameters did change meanwhile — rebuilds and binds anyway.
        if (mGiReassertPending) {
            mGiReassertPending = false;
            if (mGiPushed) mTarget->reassertGiBinding();
        }
        if (!mGiPushed || gi != mLastGi) {
            mTarget->setGlobalIllumination(gi);
            mLastGi = gi;
            mGiLightSignature = readEngineSignature();
            mGiMovableLightSignature = movableLightSig;
            mGiMovableLightsMoving = false;
            mGiMovableSettleOwed = false;
            mGiMaterialSignature = mTarget->giMaterialSignature();
            mGiPushed = true;
            mGiPendingRefresh = false;
            mGiPendingInject = false;
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

            // THE MOVING LAMPS' OWN GATE (O2). It arms the cheap re-inject and
            // NOTHING else: no stability window, no pending refresh, so a lamp
            // that moves for ever costs a re-inject every kGiLightOnlyEveryN
            // frames and not one re-solve. Like the settle gates, it is only
            // tracked while the budget is live ("GI paused" means the mirror is
            // not following the scene at all).
            if (movableLightSig != mGiMovableLightSignature && mSource->giUpdateBudget > 0) {
                mGiMovableLightSignature = movableLightSig;
                mGiMovableLightsMoving = true;
            }
            const bool matChanged = matSig != mGiMaterialSignature;
            if (matChanged && mSource->giUpdateBudget > 0) {
                // Arms the settle — and only the settle (see matSig above).
                mGiMaterialSignature = matSig;
                mGiPendingTimer.restart();
                mGiPendingRefresh = true;
                mGiStableFrames = 0;
            }
            // A MOBILITY FLIP MOVES THE SIGNATURE WITHOUT BEING A CHANGE THE
            // SETTLE MAY ANSWER (code review 2026-09-12, item 1). The engine's
            // GI geometry signature hashes the items that BOUNCE light, so an
            // object entering or leaving that set changes it — which, through
            // the gate below, would arm a pending refresh and fire a full
            // re-solve ~250 ms later. On the frame a character soft-promotes at
            // play, that re-solve IS the hitch the whole design promises cannot
            // happen (Types.h's MobilityChange::Soft, EnginePrivate.h's
            // kMovableBit); and for an authoring flip it would be a SECOND
            // rebuild on top of the one setNodeMovable already invalidated for.
            // So the flip's own frame ADOPTS the signature instead of arming.
            // Anything else that moved in the same frame keeps changing it and
            // re-arms on the next one, so nothing real is swallowed for longer
            // than one frame.
            if (sigChanged && mMobilityChanged) {
                if (mSource->giUpdateBudget > 0) mGiLightSignature = lightSig;
            } else if (sigChanged) {
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
                    if (!mGiPendingInject) mGiFramesSinceLightOnly = 0;
                    mGiPendingRefresh = true;
                    mGiPendingInject = true;      // lights/geometry: the cheap path runs
                    mGiStableFrames = 0;
                }
            } else if (mGiPendingRefresh && !matChanged) {
                ++mGiStableFrames;
            }

            // A re-solve RE-FITS the volume, so the escape term it may have been
            // armed by is 0 again the moment it returns. Re-reading the
            // signature after the rebuild and adopting it is what keeps "move a
            // cube out of the volume" costing ONE re-solve instead of two (the
            // second being the signature changing back).
            const auto adoptSignature = [&]() {
                if (vctLike) mGiLightSignature = readEngineSignature();
                mGiMaterialSignature = mTarget->giMaterialSignature();
                mGiPendingInject = false;
                // A full re-solve IS the rest frame, at the full bounce count:
                // the movable path's owed one would only redo it (F1).
                mGiMovableSettleOwed = false;
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
            } else if ((mGiPendingRefresh && mGiPendingInject) || mGiMovableLightsMoving ||
                       mGiMovableSettleOwed) {
                // Still moving: the cheap path, rate-limited. (A material-only
                // edit waits for the settle without it.) A MOVING LAMP reaches
                // this branch on its own, with no settle pending — that is the
                // whole of O2: its bounce follows it, and nothing re-solves.
                if (++mGiFramesSinceLightOnly >= kGiLightOnlyEveryN) {
                    mGiFramesSinceLightOnly = 0;
                    // IS IT STILL MOVING? A drag has its settle coming and is in
                    // motion by construction; a movable lamp is in motion while
                    // its signature is still changing. When neither is true this
                    // is the tick the latch kept alive — the REST frame, run at
                    // the scene's full bounce count (F1).
                    const bool inMotion = mGiMovableLightsMoving ||
                                          (mGiPendingRefresh && mGiPendingInject);
                    mGiMovableLightsMoving = false;   // re-armed by the next move
                    // Owe one rest tick after any moving tick, and only after a
                    // MOVABLE one: the drag path's settle does it properly.
                    mGiMovableSettleOwed = inMotion && !(mGiPendingRefresh && mGiPendingInject);
                    if (mTarget->refreshGiLighting(inMotion)) {
                        ++mGiLightRefreshCount;
                        if (!inMotion) ++mGiLightRefreshAtRestCount;
                    }
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
    // THE SUN, through the document's ONE resolver (SUN_AND_LIGHT_DEFAULTS Q1).
    // This used to be a second, private rule — "the lowest-nodeId directional"
    // — which agreed with the sky link's depth-first walk only by accident and
    // could name a different light the moment anything was re-parented.
    if (auto sun = mSource->sunLight()) return sun.data();
    // No directional light at all is NORMAL (two of the eight shipped samples):
    // Instant Radiosity still needs SOMETHING to bounce, so it falls through to
    // the lowest-nodeId light of any type, exactly as before. QHash order is
    // arbitrary, hence the explicit creation-order pick.
    iris::LightNode *any = nullptr;
    for (const auto &l : mSource->lights) {
        if (l.isNull()) continue;
        // ...but never a SKY LIGHT. It has no position and no direction to
        // trace from — it IS the ambient (SKY_LIGHT_SPEC.md §2) — and Instant
        // Radiosity given one would cast its virtual point lights from the
        // world origin. It is also the first light in a scene built from the
        // new template, so "the lowest nodeId of any type" would find it.
        if (l->lightType == iris::LightType::Sky) continue;
        if (!any || l->nodeId < any->nodeId) any = l.data();
    }
    return any;
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
    case Kind::Color:
        return skyColor == o.skyColor;
    case Kind::Realistic: {
        if (!(same(density, o.density) && same(diffusion, o.diffusion) &&
              same(horizon, o.horizon) && same(power, o.power) &&
              skyColour == o.skyColour))
            return false;
        if (hasSun != o.hasSun) return false;
        if (!hasSun) return true;
        // THE SUN'S DIRECTION, with a BAND (SKY_LIGHT_SPEC.md §3). The sun is a
        // LIGHT, so its direction arrives from a transform a keyframe or a
        // gizmo drag can nudge by a float epsilon every frame; an exact
        // comparison would re-capture the whole environment (six face renders,
        // a GGX convolution and a readback) for a rotation nobody can see.
        // dot > 1 - 1e-5 is about a quarter of a degree, well under one texel
        // of the 128^2 capture.
        const float d = sunDir.x() * o.sunDir.x() + sunDir.y() * o.sunDir.y() +
                        sunDir.z() * o.sunDir.z();
        return d > 0.99999f;
    }
    }
    return false;
}

/// The document's sky fields, read into the value applySky compares. A skyless
/// scene (or a textured one with no texture loaded) reads as Kind::None — which
/// is also the initial state. A SINGLE_COLOR sky is Kind::Color and a real sky
/// since SKY_LIGHT_SPEC §2.
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
        src.density = r.density;
        src.diffusion = r.diffusion;
        src.horizon = r.horizon;
        src.power = r.power;
        src.skyColour = r.skyColour;
        // D15: the analytic sky's sun is the SCENE'S SUN LIGHT, and there is no
        // other source for it. A document light emits down its local -Y, so the
        // direction TOWARDS the sun is the reverse of the light's travel.
        if (const auto sun = scene.sunLight()) {
            const iris::Vec3 travel = sun->getLightDir();
            if (travel.lengthSquared() > 1e-12f) {
                src.sunDir = -travel.normalized();
                src.hasSun = true;
            }
        }
    } else if (scene.skyType == iris::SkyType::SINGLE_COLOR) {
        // A SINGLE-COLOUR SKY IS A REAL SKY (SKY_LIGHT_SPEC.md §2). It used to
        // read as Kind::None — a view background with no sky pass, no
        // environment reflection and nothing for the SH integral to read, which
        // is why a colour sky could never light anything and why all seven
        // colour-sky samples leaned on the flat World ambient. It is now baked
        // as a uniform strip and taken down the equirect path like the gradient
        // sky, so its SH band 0 is exactly linear(colour), its reflections are
        // uniform, and the sun disc has a sky pass to compose over.
        src.kind = SkySource::Kind::Color;
        src.skyColor = scene.skyColor;
    }
    return src;
}

// BUILD THE DESCRIPTION, COMPARE, PUSH (ENGINEERING_DEBT_SPEC.md item 4).
//
// Two comparisons, and they answer different questions:
//   * SkySource — "is the sky made of the same things?" Its answer decides
//     whether the sky is BUILT again: an image decode and upload for an image
//     sky, a strip for a gradient, a const-buffer write for the analytic one.
//   * SkyDesc — "is this the sky the engine already has?" The ENGINE owns it:
//     Scene::setSky drops a description equal to the live one, per half, so the
//     push below is free every frame it changes nothing.
//
// WHAT IS NOT HERE ANY MORE (SKY-GPU). The environment — the reflection cube
// and the ambient SH — used to be built HERE, on the UI thread, out of the sky
// image: a 6x128^2 bilinear resample into cube faces plus a 9-band integral
// over every texel of the panorama (CPU_GPU_LIGHTING_AUDIT F2). The engine now
// CAPTURES its own sky into a cubemap on the GPU and integrates that, so this
// function's whole job is the sky itself. The Preetham bake is gone with it:
// the analytic sky is Ogre's AtmosphereNpr, five numbers pushed into a shader.
void SceneMirror::applySky(View *view)
{
    if (!mSource || !view) return;
    const SkySource src = skySourceOf(*mSource);
    if (src != mSkySource) {
        mSkySource = src;
        mSkyTexture = 0;
        mReclaimPending = true;
        for (TextureId &t : mSkyFaceTextures)  { if (t) mTarget->destroyTexture(t); t = 0; }
        // Default = no sky and NO OPINION about reflections; every failure path
        // below simply leaves it that way, which is what the old code spelled
        // out as setSky(NoSky, 0) in four places.
        mSkyDesc = SkyDesc();
        switch (src.kind) {
        case SkySource::Kind::Equirect: {
            const TextureId t = textureFor(mSource->skyTexture->source, true);
            mSkyTexture = t;   // held against reclaimUnused for as long as the sky stands
            if (t) {
                mSkyDesc.mode = SkyMode::Equirectangular;
                mSkyDesc.equirect = t;
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
            }
            break;
        }
        case SkySource::Kind::Gradient: {
            // Legacy gradientsky.frag is a pure vertical 3-stop ramp: bake it into a
            // narrow equirect strip (row 0 = zenith) and reuse the equirect sky path.
            // The ramp itself is bakeGradientSky — shared with the glTF exporter,
            // which used to carry its own copy of it (re-audit F10). It stays a
            // host bake: it is 64 x 32 texels of a linear interpolation, and
            // there is no engine-side gradient sky to adopt.
            const QImage strip = iris::bakeGradientSky(mSource->gradientTop, mSource->gradientMid,
                                                       mSource->gradientBot, mSource->gradientOffset);
            mSkyFaceTextures[0] = strip.isNull() ? 0
                : mTarget->createTexture(unsigned(strip.width()), unsigned(strip.height()),
                                         strip.constBits(), true);
            if (mSkyFaceTextures[0]) {
                mSkyDesc.mode = SkyMode::Equirectangular;
                mSkyDesc.equirect = mSkyFaceTextures[0];
            }
            break;
        }
        case SkySource::Kind::Realistic: {
            // THE ANALYTIC SKY IS THE ENGINE'S (SKY-GPU, owner pick 5). Five
            // parameters and the sun's direction; no image, no upload, no
            // debounce — the Preetham bake that used to live here cost up to
            // 524 k pixels of transcendental math per slider event.
            mSkyDesc.mode = SkyMode::Atmosphere;
            AtmosphereSky &a = mSkyDesc.atmosphere;
            a.density   = mSource->skyRealistic.density;
            a.diffusion = mSource->skyRealistic.diffusion;
            a.horizon   = mSource->skyRealistic.horizon;
            a.skyPower  = mSource->skyRealistic.power;
            // The sky's colour is a colour a user PICKS, so it is decoded like
            // every other one (§4) — the component's own numbers are linear.
            const iris::LinearColor c = iris::linearOf(mSource->skyRealistic.skyColour);
            a.skyColour = Colour(c.r, c.g, c.b, 1.0f);
            a.hasSun = src.hasSun;
            a.sunDir[0] = src.sunDir.x();
            a.sunDir[1] = src.sunDir.y();
            a.sunDir[2] = src.sunDir.z();
            break;
        }
        case SkySource::Kind::Color: {
            // A UNIFORM SKY, as a 64x32 equirect strip (SKY_LIGHT_SPEC.md §2).
            // The strip is uploaded sRGB like every other sky image, so the
            // sampler decodes it and the engine's capture re-encodes exactly
            // what it decoded — which is precisely what makes a 72-grey COLOUR
            // sky and a 72-grey PAINTED sky the same sky (the colour-space
            // rule, §4).
            const QColor c = mSource->skyColor;
            QImage strip(64, 32, QImage::Format_RGBA8888);
            strip.fill(QColor(c.red(), c.green(), c.blue(), 255));
            mSkyFaceTextures[0] = mTarget->createTexture(unsigned(strip.width()),
                                                         unsigned(strip.height()),
                                                         strip.constBits(), true);
            if (mSkyFaceTextures[0]) {
                mSkyDesc.mode = SkyMode::Equirectangular;
                mSkyDesc.equirect = mSkyFaceTextures[0];
            }
            break;
        }
        case SkySource::Kind::None:
            break;
        }
    }
    // THE SUN DISC (SKY_LIGHT_SPEC.md §3), rebuilt from the document every
    // frame and dropped by the engine's own value comparison. It rides the sky
    // description because it is drawn as part of the sky, but it is NOT part of
    // the SkySource signature: rotating the sun must move the disc without
    // re-baking anything (the bake has its own, coarser dot-product band).
    {
        SunDisc &sun = mSkyDesc.sun;
        sun = SunDisc();
        const auto sunLight = mSource->sunLight();
        if (mSource->sunDiscVisible && sunLight && sunLight->isVisibleInScene()) {
            const iris::Vec3 travel = sunLight->getLightDir();
            // A ZERO ANGULAR SIZE IS NOT A DISC, and the shader cannot draw one:
            // its edge is a smoothstep between cos(radius) and cos(0.88*radius),
            // which are the SAME number at radius 0 — undefined behaviour, and
            // in practice a full-screen flash. sunAngle 0 means "no disc".
            if (travel.lengthSquared() > 1e-12f && sunLight->sunAngle > 0.0f) {
                const iris::Vec3 toSun = -travel.normalized();
                sun.enabled = true;
                sun.dir[0] = toSun.x(); sun.dir[1] = toSun.y(); sun.dir[2] = toSun.z();
                sun.angularDiameterDeg = sunLight->sunAngle;
                sun.inProbes = mSource->sunDiscInProbes;
                // THE DISC'S RADIANCE. The sun light's colour (decoded, §4)
                // times its intensity times an overdrive: the disc must CLIP
                // white in an LDR frame and bloom under the HDR chain, which a
                // radiance of 1.0 does not do once the tonemapper has had it.
                // Not a dial — a sun that does not read as a sun is a defect,
                // not a setting (the SIZE is the dial, on the light).
                const float kDiscRadiance = 8.0f;
                const iris::LinearColor c = iris::linearOf(sunLight->color);
                const float k = kDiscRadiance * std::max(0.0f, sunLight->intensity);
                // THE SAME EFFECTIVE COLOUR THE LIGHT GETS (SUN_FOLLOWS_
                // ATMOSPHERE): a sun that looks white in the sky while lighting
                // the room orange would be the defect this toggle exists to
                // avoid, so the disc reads the identical tint.
                const Colour tint = atmosphereTintFor(sunLight.data(), sunLight.data());
                // A SUN THAT HAS SET DRAWS NO DISC (round-2 review item 4).
                // Below the horizon the tint is zero to every decimal an 8-bit
                // frame can hold, and a black disc in a night sky is a hole.
                if (sunTintIsNight(tint)) sun.enabled = false;
                sun.colour = Colour(c.r * k * tint.r, c.g * k * tint.g, c.b * k * tint.b, 1.0f);
            }
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
        // The colour sky draws its own strip now, so this is the FALLBACK the
        // frame shows where no sky pass ran (a failed upload, a headless view,
        // a transparent thumbnail's clear). Through linearOf like every other
        // colour a user picks (§4), so the fallback matches the sky it stands in
        // for instead of being 2.3x brighter than it.
        const iris::LinearColor c = iris::linearOf(mSource->skyColor);
        view->setBackground(Colour(c.r, c.g, c.b, 1.0f));
    }
}

namespace {
// Box-downsample an RGBA8888 image to at most `maxW` wide (halving until it
// fits, so every step is an exact 2x2 average). The equirect->cubemap resample
// below point-samples the result: without this a 4K sky is decimated ~30x and
// small bright features (a sun disc) alias into a crawling speckle as the sky
// changes (VISUAL_PARITY_SPEC item 3a).
//
// THIS ONE AVERAGES ENCODED BYTES, and that is correct for what it feeds: the
// cube faces it produces are UPLOADED as sRGB textures, so the average has to
// live in the same encoding the texels do.
//
// THE SKY NO LONGER COMES HERE (SKY-GPU): the engine captures its own sky into
// a cubemap on the GPU and integrates that for the ambient. What is left is the
// PER-MATERIAL reflection override — an authored equirect panorama projected
// into a cube once, at import-ish cadence rather than per sky change — which
// keeps its host-side projection deliberately: the reflection_map suite pins
// those cubes' pixels, and a GPU projection would have to prove bit-for-bit
// determinism across cold and warm texture caches to replace them.
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

// THE SIX WORLD-AXIS FACES of an equirect panorama, as engine textures the
// caller owns. Factored out of buildSkyReflection so the per-material
// reflection override (ADDENDUM A-5) builds its cube from the SAME projection
// the sky uses — a second copy of this is how one of the two ends up mirrored
// or rotated against the other.
bool SceneMirror::buildEquirectCubeFaces(const QImage &equirect, TextureId ids[6])
{
    for (int i = 0; i < 6; ++i) ids[i] = 0;
    if (equirect.isNull() || !mTarget) return false;

    const int N = 128;   // reflection cube face size; the engine box-filters its mip chain (buildCubeFromWorldFaces host chain, 2026-09-11)
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
