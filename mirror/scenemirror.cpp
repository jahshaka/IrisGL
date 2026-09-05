#include "core/math/mat3.h"
#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "irisgl/mirror/scenemirror.h"

#include <cstring>
#include <algorithm>
#include <cmath>
#include <type_traits>

#include "irisgl/document/scenegraph/nodegraph.h"
#include "irisgl/document/scenegraph/scene.h"
#include "irisgl/document/scenegraph/scenenode.h"
#include "irisgl/document/scenegraph/meshnode.h"
#include "irisgl/document/scenegraph/lightnode.h"
#include "irisgl/document/scenegraph/decalnode.h"
#include "irisgl/document/scenegraph/cameranode.h"
#include "irisgl/document/scenegraph/particlesystemnode.h"
#include "irisgl/document/assets/mesh.h"
#include "irisgl/document/assets/skeleton.h"
#include "irisgl/document/animation/animation.h"
#include "irisgl/document/animation/clipextractor.h"
#include "irisgl/document/assets/vertexlayout.h"
#include "irisgl/document/assets/vertexbuffer.h"     // VertexBuffer / IndexBuffer (CPU copies)
#include "irisgl/document/materials/material.h"
#include "irisgl/document/materials/pbrmaterial.h"
#include "irisgl/document/materials/defaultmaterial.h"
#include "irisgl/document/materials/custommaterial.h"
#include "irisgl/core/properties/property.h"
#include "irisgl/core/math/trs.h"
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
    Hasher &operator<<(const QColor &c) { return *this << c.rgba(); }
    Hasher &operator<<(const iris::Vec3 &v) { return *this << v.x() << v.y() << v.z(); }
};
}

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
    for (TextureId t : mIconTextures) mTarget->destroyTexture(t);
    mIconTextures.clear();
    // Decal-atlas slices are a FIXED, process-wide budget (32 slices), and this
    // map used to survive setSource: open five worlds with decals and the atlas
    // was full, after which every decal in the session silently projected
    // nothing. The atlas is refcounted, so releasing our references here is
    // enough — a slice another scene still holds stays alive.
    for (TextureId t : mDecalTextures) if (t) mTarget->destroyTexture(t);
    mDecalTextures.clear();
    mTarget->setSky(SkyMode::NoSky, 0);   // also clears the engine's reflection cubemap
    for (TextureId &t : mSkyFaceTextures)  { if (t) mTarget->destroyTexture(t); t = 0; }
    for (TextureId &t : mReflFaceTextures) { if (t) mTarget->destroyTexture(t); t = 0; }
    mSkyKind = SkyKind::None;
    mSkyHash = 0;
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
    // The document's graph is about to migrate out of our manager: everything
    // we attached to its nodes must go first, while those nodes still exist.
    // (The 2026-09-05 player→editor faults: particle systems and the planar
    // pass reading nodes the migration had already destroyed.)
    for (Entry &e : mEntries) releaseEntry(e);
    mEntries.clear();
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
    // Per-material work is memoised for the duration of this walk (see
    // MaterialSync): every mesh node sharing a material used to pay for it.
    mMaterialSync.clear();
    mAnyShadowCaster = false;
    mAnyRefractive = false;
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
    syncClips();
    syncHighlight();
    syncGrid();
    return mVisited;
}

MeshId SceneMirror::engineMesh(iris::Mesh *mesh) const
{
    auto it = mMeshes.constFind(mesh);
    return it == mMeshes.constEnd() ? 0 : it.value();
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

void SceneMirror::setHighlightedNode(iris::SceneNodePtr node)
{
    mHighlighted = node;
}

void SceneMirror::setHighlightWireframe(bool on)
{
    mHighlightWireframe = on;
}

void SceneMirror::collectHighlightMeshes(const iris::SceneNodePtr &node,
                                         std::vector<std::pair<iris::MeshNode *, MeshId>> &out)
{
    if (!node || !node->isVisible()) return;
    if (node->getSceneNodeType() == iris::SceneNodeType::Mesh) {
        auto meshNode = static_cast<iris::MeshNode *>(node.data());
        if (iris::Mesh *mesh = meshNode->getMesh().data())
            if (MeshId m = engineMesh(mesh)) out.emplace_back(meshNode, m);
    }
    const int n = node->childCount();
    for (int i = 0; i < n; ++i)
        if (iris::SceneNode *c = node->childAt(i))
            collectHighlightMeshes(c->sharedFromThis(), out);
}

void SceneMirror::syncHighlight()
{
    // Every mesh under the highlighted node, the node itself included: selecting
    // an asset's ROOT (or any group) outlines the whole asset, not just one part.
    std::vector<std::pair<iris::MeshNode *, MeshId>> targets;
    if (mHighlighted) collectHighlightMeshes(mHighlighted, targets);
    if (targets.empty()) {
        for (HighlightShell &s : mHighlightShells) { if (s.node) mTarget->setNodeVisible(s.node, false); s.mesh = 0; }
        mReclaimPending = true;
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
    MaterialId mat;
    if (mHighlightWireframe) {
        if (!mHighlightMaterial)
            mHighlightMaterial = mTarget->createUnlitMaterial(kSelection, false, true);   // on top, wireframe
        mat = mHighlightMaterial;
    } else {
        if (!mOutlineMaterial)
            mOutlineMaterial = mTarget->createOutlineMaterial(kSelection);
        mat = mOutlineMaterial;
    }
    // Live colour changes (preference edited with a selection active): both
    // highlight materials are unlit, so one setter updates each in place.
    if (pref != mHighlightColourApplied) {
        mHighlightColourApplied = pref;
        if (mHighlightMaterial) mTarget->setUnlitMaterial(mHighlightMaterial, kSelection);
        if (mOutlineMaterial)   mTarget->setUnlitMaterial(mOutlineMaterial, kSelection);
    }
    if (!mat) return;
    // One pooled shell per target mesh; extra shells from a previous (larger)
    // selection are hidden, not destroyed.
    if (mHighlightShells.size() < targets.size()) mHighlightShells.resize(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        iris::MeshNode *meshNode = targets[i].first;
        const MeshId m = targets[i].second;
        HighlightShell &s = mHighlightShells[i];
        if (!s.node) s.node = mTarget->createNode();
        if (!s.node) continue;
        if (s.mesh != m || s.wireframe != mHighlightWireframe) {
            if (mTarget->attachMesh(s.node, m, mat)) {
                s.mesh = m;
                s.wireframe = mHighlightWireframe;
                mReclaimPending = true;   // the shell's previous mesh may be free
            }
        }
        // The outline is the same mesh scaled up slightly around the node's pivot:
        // only the band where the shell pokes out past the original is visible.
        // Band thickness follows the Preferences "outline width" the same way the
        // colour does (scene->outlineWidth, pushed by MainWindow): width/150 maps
        // the historical default 6 to the historical 1.04 hull; today's default 3
        // gives 1.02 — half the band. <=0 (never pushed) falls back to the default.
        iris::Mat4 t = meshNode->getGlobalTransform();
        if (!mHighlightWireframe) {
            const int w = (mSource && mSource->outlineWidth > 0) ? mSource->outlineWidth : 3;
            t.scale(1.0f + float(w) / 150.0f);
        }
        pushTransform(mTarget, s.node, t);
        mTarget->setNodeVisible(s.node, true);
    }
    for (size_t i = targets.size(); i < mHighlightShells.size(); ++i) {
        HighlightShell &s = mHighlightShells[i];
        if (s.node) mTarget->setNodeVisible(s.node, false);
        s.mesh = 0;
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

void SceneMirror::syncCameraWires(Entry &e, iris::CameraNode *camera)
{
    using jahshaka::engine::Vec3;
    const bool wanted = mCameraBodies && camera->bodyVisible &&
                        camera != mViewCamera;   // never draws itself — see applyCamera
    if (!wanted) {
        if (e.wireNode) mTarget->setNodeVisible(e.wireNode, false);
        return;
    }
    if (!e.wireNode) e.wireNode = mTarget->createNode(e.node);
    if (!e.wireNode) return;

    const bool selected = mHighlighted &&
                          mHighlighted.data() == static_cast<iris::SceneNode *>(camera);
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

void SceneMirror::setGrid(bool visible, float spacing)
{
    mGridVisible = visible;
    // Sanitise: the spacing is the editor's snap size; refuse degenerate values.
    mGridSpacing = std::min(std::max(spacing, 0.01f), 100.0f);
}

void SceneMirror::setGridExtent(float extent)
{
    mGridExtent = std::min(std::max(extent, 1.0f), 100000.0f);
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
    if (!mGridNode) {
        mGridNode = mTarget->createNode();
        if (!mGridNode) return;
        mGridMinorNode = mTarget->createNode(mGridNode);
        mGridMajorNode = mTarget->createNode(mGridNode);
        // A hair below y=0 so floor geometry sitting on the plane (the default
        // ground is at +1e-4) occludes the grid cleanly instead of z-fighting.
        mTarget->setNodeTransform(mGridNode, Vec3(0, -0.01f, 0), Quat(), Vec3(1, 1, 1));
        // Unlit (never fogged), depth-tested (occluded by geometry), blended.
        mGridMinorMaterial = mTarget->createUnlitMaterial(mGridMinorColour, true);
        mGridMajorMaterial = mTarget->createUnlitMaterial(mGridMajorColour, true);
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
        if (e.wireNode) mTarget->setNodeVisible(e.wireNode, false);
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
    const bool selected = mHighlighted &&
                          mHighlighted.data() == static_cast<iris::SceneNode *>(light);
    int shape = kind;
    if (!selected) {
        if (kind == 1) shape = -1;        // point: rings are the falloff volume
        else if (kind == 2) shape = 0;    // spot: keep the arrow, drop the cone
    }
    if (!e.wireNode) e.wireNode = mTarget->createNode(e.node);
    if (!e.wireNode) return;
    if (shape < 0) {
        if (e.wireKind != -1) { mTarget->detachMesh(e.wireNode); e.wireKind = -1; }
        mTarget->setNodeVisible(e.wireNode, true);   // the icon set rides this node
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
    mTarget->setNodeTransform(e.wireNode, Vec3(), Quat(),
                              Vec3(rx * (s.x() > 1e-6f ? 1.0f / s.x() : 1.0f),
                                   ry * (s.y() > 1e-6f ? 1.0f / s.y() : 1.0f),
                                   rz * (s.z() > 1e-6f ? 1.0f / s.z() : 1.0f)));
    mTarget->setNodeVisible(e.wireNode, true);
    syncLightIcon(e, light);
}

// The icon billboard: one camera-facing glyph at the light's position (sun for
// directional, bulb for point, spotlight for spot — like Unreal's sprites). It
// rides the wireNode so the light-wires toggle and node teardown govern it, but
// instance positions are world-space (the set hangs off the engine's static
// root). Engine-side only: document picking never sees it.
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
        if (!mTarget->createBillboardSet(e.wireNode, iconTextureFor(path), false, 1))
            return;
        e.hasIcon = true;
        e.iconSignature = path;
    }
    BillboardInstance b;
    const iris::Vec3 p = light->getGlobalPosition();
    b.position = Vec3(p.x(), p.y(), p.z());
    b.size = light->iconSize > 0.0f ? light->iconSize : 0.5f;
    mTarget->setBillboards(e.wireNode, &b, 1);
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
    TextureId id = mTarget->createTexture(unsigned(img.width()), unsigned(img.height()), img.constBits(), true);
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
        if (mesh && (!e.hasMesh || e.materialPtr != material || e.meshPtr != mesh)) {
            MeshId m = meshFor(mesh);
            MaterialId mat = materialFor(material);
            bool attached = false;
            e.gpuSkinned = false;
            e.boneCount = 0;
            if (m && mat && !e.skeleton.isNull()) {
                // GPU skinning: a SEPARATE entry point, because the engine has to
                // know the mesh is skinned before the renderable exists —
                // attaching first and rigging later yields a silently unskinned
                // character.
                SkeletonDesc rig;
                if (toSkeletonDesc(e.skeleton, rig) &&
                    mTarget->attachSkinnedMesh(e.node, m, mat, rig)) {
                    attached = true;
                    e.gpuSkinned = true;
                    e.boneCount = rig.bones.size();
                    e.rigId = rig.id;
                    e.clipSignature.clear();     // force a clip re-attach
                }
            }
            // Anything the engine would not rig (an over-limit rig, a mesh whose
            // bone buffers went missing) still renders — at bind pose, unskinned.
            // One renderer, never two.
            if (!attached && m && mat) attached = mTarget->attachMesh(e.node, m, mat);
            if (attached) {
                e.hasMesh = true; e.material = mat; e.materialPtr = material; e.mesh = m; e.meshPtr = mesh;
                mReclaimPending = true;   // the old mesh/material may now be unreferenced
                e.texturesPushed = false;
                e.pbrPushed = false;
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
            e.pbrPushed = false;
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
                if (!e.pbrPushed || !(ms.pbr == e.lastPbr)) {
                    if (mTarget->setPbrMaterial(e.material, ms.pbr)) {
                        e.lastPbr = ms.pbr;
                        e.pbrPushed = true;
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
        if (mTarget->setLight(e.node, toLightDesc(light))) e.hasLight = true;
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
        syncCameraWires(e, static_cast<iris::CameraNode *>(node));
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
       << ps->useAdditive << ps->alphaHash << ps->randomRotation
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
    // until the sky changes — mSkyKind/mSkyHash own that lifetime, not this.
    if (mSkyTexture) usedTextures.insert(mSkyTexture);
    for (auto it = mTextures.begin(); it != mTextures.end();) {
        if (usedTextures.contains(it.value())) { ++it; continue; }
        mTarget->destroyTexture(it.value()); it = mTextures.erase(it);
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
    for (auto it = mEntries.begin(); it != mEntries.end();) {
        if (it->lastSeen == mSyncStamp) { ++it; continue; }
        releaseEntry(*it);
        it = mEntries.erase(it);
    }
}

MeshId SceneMirror::meshFor(iris::Mesh *mesh)
{
    auto it = mMeshes.constFind(mesh);
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
    if (id) mMeshes.insert(mesh, id);
    return id;
}

MaterialId SceneMirror::materialFor(iris::Material *material)
{
    PbrParams p;
    if (!material || !toPbrParams(material, p)) {
        // Unknown material kinds (CustomMaterial shader graphs, matcap, glass...) get
        // one shared neutral material until they have an engine equivalent.
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
    if (id) mMaterials.insert(material, id);
    return id;
}

/// Refraction "Auto" (POST_CHAIN_SPEC §9.5) needs to know whether the scene HAS
/// a refractive material right now: the chain only grows its second scene pass
/// and its full-res copy while one exists, so the cost when unused is exactly
/// zero. Accumulated as materials are visited, consumed by applyEnvironment,
/// reset by sync() — the same shape as mAnyShadowCaster.
void SceneMirror::noteRefractive(const PbrParams &p)
{
    if (p.alphaMode == PbrAlphaMode::Refractive) mAnyRefractive = true;
}

TextureId SceneMirror::textureFor(const QString &path, bool srgb)
{
    auto it = mTextures.constFind(path);
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
    if (!QFileInfo::exists(path)) return 0;
    TextureId id = mTarget->loadTexture(path.toStdString(), srgb);
    if (id) mTextures.insert(path, id);
    return id;
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
    // PbrMaterial's "u_occlusionMap" is deliberately NOT mapped: the engine has no
    // ambient-occlusion slot (HlmsPbs limitation, see engine Types.h).
    struct Slot { QLatin1StringView name; PbrTextureSlot slot; bool srgb; };
    static const Slot kSlots[] = {
        { QLatin1StringView("u_baseColorMap"),  PbrTextureSlot::Albedo,    true  },
        { QLatin1StringView("u_diffuseTexture"), PbrTextureSlot::Albedo,   true  },
        { QLatin1StringView("u_normalMap"),     PbrTextureSlot::Normal,    false },
        { QLatin1StringView("u_normalTexture"), PbrTextureSlot::Normal,    false },
        { QLatin1StringView("u_metallicMap"),   PbrTextureSlot::Metalness, false },
        { QLatin1StringView("u_roughnessMap"),  PbrTextureSlot::Roughness, false },
        { QLatin1StringView("u_emissiveMap"),   PbrTextureSlot::Emissive,  true  },
    };
    // Resolve every candidate path: the textures map (Texture2D::source), then
    // shader-graph texture properties (a file path in the property value).
    for (const Slot &sl : kSlots) {
        auto tit = material->textures.constFind(sl.name);
        if (tit != material->textures.constEnd() && tit.value() && !tit.value()->source.isEmpty())
            ms.binds.push_back({ sl.slot, tit.value()->source, sl.srgb });
    }
    if (auto *custom = dynamic_cast<iris::CustomMaterial *>(material)) {
        for (iris::Property *prop : custom->properties) {
            if (!prop || prop->type != iris::PropertyType::Texture) continue;
            const QString path = prop->getValue().toString();
            if (path.isEmpty()) continue;
            if (prop->name == "diffuseTexture" || prop->name == "baseColorMap" || prop->name == "albedoMap")
                ms.binds.push_back({ PbrTextureSlot::Albedo, path, true });
            else if (prop->name == "normalTexture" || prop->name == "normalMap")
                ms.binds.push_back({ PbrTextureSlot::Normal, path, false });
            else if (prop->name == "emissiveMap")
                ms.binds.push_back({ PbrTextureSlot::Emissive, path, true });
        }
    }
    // Which slot gets which file, as one hash: the whole job of this number is
    // to let a mesh conclude "unchanged" in one comparison.
    Hasher hs;
    hs << quint32(ms.binds.size());
    for (const TextureBind &b : ms.binds) hs << int(b.slot) << b.path;
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
    bool bound[5] = { false, false, false, false, false };
    TextureId boundIds[5] = { 0, 0, 0, 0, 0 };
    for (const TextureBind &b : binds) {
        if (bound[int(b.slot)]) continue;
        TextureId t = textureFor(b.path, b.srgb);
        if (t && mTarget->setPbrTexture(e.material, b.slot, t)) {
            bound[int(b.slot)] = true;
            boundIds[int(b.slot)] = t;
        }
    }
    for (int i = 0; i < 5; ++i) if (!bound[i]) mTarget->setPbrTexture(e.material, PbrTextureSlot(i), 0);
    // What reclaimUnused needs: the IDS, not the paths. A world switch or a
    // material edit that drops a map leaves the engine texture referenced by
    // nobody, and before this the mirror simply never freed one.
    e.boundTextures.assign(boundIds, boundIds + 5);
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
        out.uvScale         = pbr->textureScale;
        out.twoSided        = pbr->renderStates.rasterState.cullMode == iris::CullMode::None;
        // occlusionMap/occlusionFactor: no engine equivalent (HlmsPbs has no AO
        // slot — see Types.h); intentionally dropped, not faked.
        return true;
    }
    if (auto *custom = dynamic_cast<iris::CustomMaterial *>(material)) {
        // Effects-module materials (Default/Flat/... .shader): read the properties the
        // shader graph exposes. Colour → albedo, shininess → roughness. Textures are
        // bound by syncTextures() from the texture properties.
        bool haveColour = false, haveRoughness = false; float shininess = 20.0f;
        out.albedo = Colour(0.8f, 0.8f, 0.8f); out.metalness = 0.0f; out.emissive = Colour(0, 0, 0);
        for (iris::Property *prop : custom->properties) {
            if (!prop) continue;
            const QVariant v = prop->getValue();
            if (prop->type == iris::PropertyType::Color &&
                (prop->name == "diffuseColor" || prop->name == "color" || prop->name == "albedo" || prop->name == "baseColor")) {
                const QColor c = v.value<QColor>();
                out.albedo = Colour(c.redF(), c.greenF(), c.blueF(), 1.0f); haveColour = true;
            } else if (prop->type == iris::PropertyType::Float && prop->name == "shininess") {
                shininess = v.toFloat();
            } else if (prop->type == iris::PropertyType::Float && (prop->name == "roughness" || prop->name == "roughnessFactor")) {
                out.roughness = v.toFloat(); haveRoughness = true;
            } else if (prop->type == iris::PropertyType::Float && (prop->name == "metallic" || prop->name == "metalness")) {
                out.metalness = v.toFloat();
            } else if (prop->type == iris::PropertyType::Float && prop->name == "textureScale") {
                out.uvScale = v.toFloat();
            }
        }
        if (!haveRoughness) {
            // Only DERIVE roughness from shininess when the material carries no
            // real roughness. This line used to run unconditionally, stamping
            // over any read value — and since assimp encodes glTF roughness as
            // shininess = (1-r)^2*1000, the 128 clamp turned every reasonably
            // smooth imported material into a roughness-0.1 near-mirror.
            const float shin = std::max(0.0f, std::min(shininess, 128.0f));
            out.roughness = 1.0f - std::sqrt(shin / 128.0f) * 0.9f;
        }
        (void)haveColour;
        return true;
    }
    if (auto *def = dynamic_cast<iris::DefaultMaterial *>(material)) {
        // Legacy Blinn-Phong material: diffuse -> albedo, shininess -> roughness.
        const QColor c = def->getDiffuseColor();
        out.albedo    = Colour(c.redF(), c.greenF(), c.blueF(), 1.0f);
        out.metalness = 0.0f;
        const float shin = std::max(0.0f, std::min(def->getShininess(), 128.0f));
        out.roughness = 1.0f - std::sqrt(shin / 128.0f) * 0.9f;
        out.emissive  = Colour(0, 0, 0);
        out.uvScale   = def->getTextureScale();
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
    if (!e.wireNode) e.wireNode = mTarget->createNode(e.node);
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

}  // namespace

void SceneMirror::attachClipsFor(Entry &e)
{
    if (!e.gpuSkinned || !e.docNode || e.skeleton.isNull()) return;
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
    e.lastClipName.clear();
    e.lastClipTime = -1.0f;
    if (clips.isEmpty() || !host) { e.clipMap.clear(); e.clipIdMap.clear(); return; }

    // R2 again, from the host side: clips ACCUMULATE on a node — the Avatar
    // page loads a Mixamo animation onto an already-loaded character — so this
    // runs while the previous set is attached AND, quite possibly, playing.
    // Ogre's addAnimationsFromSkeleton would go stale every active-animation
    // pointer, so the engine refuses; disable everything first. (Found by
    // scripting.e2e.avatar the moment a cross-file clip was added: the
    // character froze at bind pose with one warning in the log.)
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
                                          e.skeleton, clips[i]->getSkeletalAnimation(),
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
    if (descs.empty()) { e.clipMap.clear(); e.clipIdMap.clear(); return; }

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
}

bool SceneMirror::entryBoneWorldTransforms(const Entry &e, QHash<QString, iris::Mat4> &out) const
{
    if (!mTarget) return false;
    if (!e.gpuSkinned || e.skeleton.isNull() || !e.docNode) return false;
    const QList<iris::BonePtr> &bones = e.skeleton->bones;
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
    mDerivedScratch.assign(size_t(bones.size()), iris::Mat4());
    mDerivedDone.assign(size_t(bones.size()), 0);
    std::vector<iris::Mat4> &derived = mDerivedScratch;
    std::vector<char> &done = mDerivedDone;
    std::function<iris::Mat4(int)> resolve = [&](int i) -> iris::Mat4 {
        if (done[i]) return derived[i];
        done[i] = 1;                       // cycles are impossible by rig contract; guard anyway
        const BonePose &p = poses[size_t(i)];
        iris::Mat4 local;
        local.translate(iris::Vec3(p.position.x, p.position.y, p.position.z));
        local.rotate(iris::Quat(p.rotation.w, p.rotation.x, p.rotation.y, p.rotation.z));
        local.scale(iris::Vec3(p.scale.x, p.scale.y, p.scale.z));
        int parent = -1;
        if (!bones[i]->parentBone.isNull()) {
            const auto pit = e.skeleton->boneMap.constFind(bones[i]->parentBone->name);
            if (pit != e.skeleton->boneMap.constEnd() && pit.value() != i) parent = pit.value();
        }
        derived[i] = parent >= 0 ? resolve(parent) * local : local;
        return derived[i];
    };
    for (int i = 0; i < bones.size(); ++i) out.insert(bones[i]->name, meshWorld * resolve(i));
    return true;
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
    // Read the pose the last rendered frame produced, then move whatever rides
    // it — which is why this runs at the TOP of sync(), before the walk that
    // pushes transforms to the engine. The one-frame lag is inherent (see
    // document/scenegraph/socket.h) and is not worth an extra engine update to
    // close.
    if (!mSource) return 0;
    return mSockets.resolve(mSource.data());
}

void SceneMirror::syncClips()
{
    // The per-frame animation cost, all of it: one small struct per skinned
    // node, pushed only when the clip or the time actually moved. No matrix
    // decompositions, no vertices, no uploads — the sampling, the blending and
    // the FK all happen inside the engine's threaded update.
    if (!mSource) return;
    const float t = mSource->animationTime();
    for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
        Entry &e = *it;
        if (!e.gpuSkinned || !e.docNode) continue;
        attachClipsFor(e);
        if (e.clipMap.isEmpty()) continue;

        iris::SceneNode *host = clipHostOf(e.docNode);
        iris::AnimationPtr active = host ? host->getAnimation() : iris::AnimationPtr();
        // A node with clips attached but none active keeps its BIND pose: with
        // no active animation SkeletonInstance::update() does not even reset to
        // pose, so "no clip" is a frozen pose by construction, and the pose it
        // is frozen at is the one the rig was created with.
        if (active.isNull() || !active->hasSkeletalAnimation()) {
            if (!e.lastClipName.isEmpty()) {
                mTarget->setClipStates(e.node, nullptr, 0);
                e.lastClipName.clear();
                e.lastClipTime = -1.0f;
            }
            continue;
        }
        const auto mappedId = e.clipMap.constFind(active.data());
        if (mappedId == e.clipMap.constEnd()) continue;
        const auto mapped = e.clipIdMap.constFind(mappedId.value());
        if (mapped == e.clipIdMap.constEnd()) continue;
        const QString name = mapped.value();
        const bool looping = active->getLooping();
        if (name == e.lastClipName && qFuzzyCompare(t + 1.0f, e.lastClipTime + 1.0f) &&
            looping == e.lastClipLooping)
            continue;

        ClipState state;
        state.name = name.toStdString();
        state.enabled = true;
        // ABSOLUTE time, always — the document owns the clock and the engine
        // does its own wrap (fmod when looping, clamp when not), which is
        // exactly what Animation::getSampleTime does document-side.
        state.time = t;
        state.weight = 1.0f;
        state.looping = looping;
        if (mTarget->setClipStates(e.node, &state, 1)) {
            e.lastClipName = name;
            e.lastClipTime = t;
            e.lastClipLooping = looping;
        }
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

void SceneMirror::applyEnvironment(View *view, Engine *engine)
{
    if (!mSource || !view) return;
    // Shadow filter: the engine has ONE global filter (Engine.h), the document a
    // per-light ShadowMapType. Policy: the strongest (softest) quality any
    // shadow-casting light asked for wins, as accumulated by the last sync().
    // Nothing casting shadows leaves the engine's current filter untouched.
    // World panel "Shadow Softness" (scene->shadowFilterTier, POST_CHAIN_SPEC
    // §9.3) OVERRIDES that derivation outright when it is >= 0 — including for
    // scenes with no shadow caster yet, exactly the way shadowResolution's
    // override below works.
    if (engine) {
        const int tier = mSource->shadowFilterTier;
        if (tier >= 0) {
            const ShadowFilter wanted = tier >= 2 ? ShadowFilter::VerySoft
                                      : tier == 1 ? ShadowFilter::Soft
                                                  : ShadowFilter::Hard;
            if (engine->shadowFilter() != wanted) engine->setShadowFilter(wanted);
        } else if (mAnyShadowCaster && engine->shadowFilter() != mShadowFilter) {
            engine->setShadowFilter(mShadowFilter);
        }
        // Particle simulation clock (PARTICLES_FX2_SPEC §10.3). PROCESS-WIDE in
        // the renderer — one frame-time source, no per-scene and no per-node
        // clock exists — so the last scene to call applyEnvironment owns it.
        // Offscreen thumbnail and preview scenes never call this, so they never
        // fight the editor for it. Pushed only on change: setParticleTimeScale
        // and setFixedFrameDelta cancel each other inside the backend, and the
        // suites hold a fixed step across whole runs.
        // ...unless a FIXED frame delta is live (a scripted editor.frame(n, dt)
        // or a test): the two settings cancel each other inside the backend, so
        // pushing the scale here would silently undo the caller's determinism
        // every frame.
        if (engine->fixedFrameDelta() <= 0.0f &&
            engine->particleTimeScale() != mSource->particleTimeScale)
            engine->setParticleTimeScale(mSource->particleTimeScale);
    }
    // Shadow Size: one global atlas, so the per-light combo is only a REQUEST
    // and the largest one wins. World panel "Shadow Quality" (scene->
    // shadowResolution, VISUAL_PARITY item 2 option A) overrides that
    // derivation outright when it is non-zero — including for scenes with no
    // shadow caster yet, so the setting is what the user asked for and not a
    // function of the light list. The engine rebuilds its shadow atlas on
    // change; the compare here is what keeps that rare.
    if (engine) {
        const unsigned wanted = mSource->shadowResolution > 0
                                    ? unsigned(qBound(256, mSource->shadowResolution, 8192))
                                    : (mAnyShadowCaster ? mMaxShadowResolution : 0u);
        if (wanted > 0 && engine->shadowResolution() != wanted)
            engine->setShadowResolution(wanted);
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
        fx.bloom          = mSource->bloomEnabled;
        fx.bloomThreshold = mSource->bloomThreshold;
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
        iris::LightNode *driver = gi.mode == GiMode::InstantRadiosity ? resolveGiLight() : nullptr;
        gi.irLight = driver ? engineNode(driver) : 0;
        const auto same = [](const GiParams &a, const GiParams &b) {
            return a.mode == b.mode && a.quality == b.quality && a.irLight == b.irLight &&
                   a.numBounces == b.numBounces &&
                   a.pccProbesX == b.pccProbesX && a.pccProbesY == b.pccProbesY &&
                   a.pccProbesZ == b.pccProbesZ &&
                   a.boundsMin.x == b.boundsMin.x && a.boundsMin.y == b.boundsMin.y &&
                   a.boundsMin.z == b.boundsMin.z && a.boundsMax.x == b.boundsMax.x &&
                   a.boundsMax.y == b.boundsMax.y && a.boundsMax.z == b.boundsMax.z;
        };
        // What a refresh should track depends on the mode: IR re-traces from
        // ONE driving light, so only that light's transform matters; VCT
        // injects EVERY light into the voxel volume, so any light moving (or
        // appearing/dying) goes stale until a re-voxelize.
        iris::Mat4 lightWorld;
        if (driver) {
            lightWorld = driver->getGlobalTransform();
        } else if (gi.mode == GiMode::Vct || gi.mode == GiMode::VctPccHybrid) {
            for (const auto &l : mSource->lights)
                if (!l.isNull()) lightWorld *= l->getGlobalTransform();   // cheap combined signature
        }
        if (!mGiPushed || !same(gi, mLastGi)) {
            mTarget->setGlobalIllumination(gi);
            mLastGi = gi;
            mGiLightWorld = lightWorld;
            mGiPushed = true;
            ++mGiPushCount;
        } else if (gi.mode != GiMode::Off && mSource->giAutoRefresh &&
                   lightWorld != mGiLightWorld) {
            // IR re-traces in milliseconds; VCT re-injects + re-voxelizes on the
            // GPU (a few ms at editor volumes on real hardware). The per-frame
            // signature compare is the debounce, as for the push above — and the
            // debounce is load-bearing: this branch firing every frame is a whole
            // VCT rebuild per frame, which is invisible in the picture and fatal
            // to the frame rate. giRefreshCount() is what proves it does not.
            mGiLightWorld = lightWorld;
            mTarget->refreshGlobalIllumination();
            ++mGiRefreshCount;
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

void SceneMirror::applySky(View *view)
{
    if (!mSource || !view) return;
    // WHICH sky (the dispatch below switches on it, and the realistic-bake
    // debounce asks whether the previous sky was realistic too) plus a hash of
    // the values it is built from. This used to be one QString built with
    // startsWith() dispatch — up to ten QString::arg calls per frame whose
    // only purpose was an equality test (deep audit 2026-09, area 8).
    SkyKind kind = SkyKind::None;
    Hasher hs;
    if (mSource->skyType == iris::SkyType::EQUIRECTANGULAR && mSource->skyTexture) {
        kind = SkyKind::Equirect;
        hs << mSource->skyTexture->source;
    } else if (mSource->skyType == iris::SkyType::CUBEMAP && mSource->skyTexture &&
               mSource->skyTexture->isCubeMap()) {
        kind = SkyKind::Cubemap;
        hs << reinterpret_cast<quintptr>(mSource->skyTexture.data());
    } else if (mSource->skyType == iris::SkyType::GRADIENT) {
        kind = SkyKind::Gradient;
        hs << mSource->gradientTop << mSource->gradientMid << mSource->gradientBot
           << mSource->gradientOffset;
    } else if (mSource->skyType == iris::SkyType::REALISTIC) {
        kind = SkyKind::Realistic;
        const iris::SkyRealistic &s = mSource->skyRealistic;
        // Sky Detail (the bake width) rides in the signature: changing it must
        // re-bake exactly like changing a scattering parameter does.
        // HDR rides in the signature too: with the post chain on, the bake stops
        // before its own tonemap (POST_CHAIN_SPEC §7.1), so toggling HDR must
        // re-bake exactly like changing a scattering parameter does.
        hs << s.luminance << s.reileigh << s.mieCoefficient << s.mieDirectionalG
           << s.turbidity << s.sunPosX << s.sunPosY << s.sunPosZ
           << mSource->skyBakeResolution << mSource->hdrEnabled;
    }
    // A skyless scene hashes to 0, not to the FNV basis: that keeps the initial
    // (None, 0) state EQUAL to "no sky", so a single-colour sky does not push a
    // redundant setSky(NoSky) on its first frame the way a non-zero empty hash
    // would. (The old code compared two empty QStrings and got the same answer.)
    const quint64 signature = kind == SkyKind::None ? 0 : hs.h;
    if (kind != mSkyKind || signature != mSkyHash) {
        // Debounce the realistic bake: a slider drag changes the 8 parameters on
        // every event, and the Preetham bake is per-pixel CPU math. Re-bake at
        // most every 150 ms — applySky recomputes the signature next frame, so
        // the final value always lands once the slider settles.
        if (kind == SkyKind::Realistic && mSkyKind == SkyKind::Realistic &&
            mRealisticBakeTimer.isValid() && mRealisticBakeTimer.elapsed() < 150)
            return;
        mSkyKind = kind;
        mSkyHash = signature;
        mSkyTexture = 0;
        mReclaimPending = true;
        for (TextureId &t : mSkyFaceTextures)  { if (t) mTarget->destroyTexture(t); t = 0; }
        for (TextureId &t : mReflFaceTextures) { if (t) mTarget->destroyTexture(t); t = 0; }
        // The ambient integral belongs to the sky that is about to be built:
        // drop the old one first so a failed build cannot leave a stale colour.
        clearSkyAmbient();
        if (kind == SkyKind::Equirect) {
            TextureId t = textureFor(mSource->skyTexture->source, true);
            mSkyTexture = t;   // held against reclaimUnused for as long as the sky stands
            mTarget->setSky(t ? SkyMode::Equirectangular : SkyMode::NoSky, t);
            // Cubemap skies feed environment reflections (IBL); give equirect
            // skies the same by resampling the image into six small faces.
            if (t) applySkyReflection(QImage(mSource->skyTexture->source));
        } else if (kind == SkyKind::Cubemap) {
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
                mTarget->setSkyCubemap(mSkyFaceTextures);
                // A cubemap sky never passes through applySkyReflection (the
                // engine takes the faces straight): integrate them here so it
                // drives ambient like every other textured sky (item 3b).
                recordCubeAmbientSh(faces);
            } else {
                mTarget->setSky(SkyMode::NoSky, 0);
            }
        } else if (kind == SkyKind::Gradient) {
            // Legacy gradientsky.frag is a pure vertical 3-stop ramp: bake it into a
            // narrow equirect strip (row 0 = zenith) and reuse the equirect sky path.
            const float middle = qBound(0.01f, mSource->gradientOffset, 0.99f);
            const QColor top = mSource->gradientTop, mid = mSource->gradientMid, bot = mSource->gradientBot;
            const int H = 256, W = 4;
            std::vector<unsigned char> px(size_t(W) * H * 4u);
            for (int r = 0; r < H; ++r) {
                const float offset = 1.0f - float(r) / (H - 1);   // 1 at the top row
                float t; const QColor *c0, *c1;
                if (offset <= middle) { t = offset / middle;                 c0 = &bot; c1 = &mid; }
                else                  { t = (offset - middle) / (1 - middle); c0 = &mid; c1 = &top; }
                const unsigned char rr = (unsigned char)qBound(0.0f, (c0->redF()   + (c1->redF()   - c0->redF())   * t) * 255.0f, 255.0f);
                const unsigned char gg = (unsigned char)qBound(0.0f, (c0->greenF() + (c1->greenF() - c0->greenF()) * t) * 255.0f, 255.0f);
                const unsigned char bb = (unsigned char)qBound(0.0f, (c0->blueF()  + (c1->blueF()  - c0->blueF())  * t) * 255.0f, 255.0f);
                for (int x = 0; x < W; ++x) {
                    unsigned char *p = &px[(size_t(r) * W + x) * 4u];
                    p[0] = rr; p[1] = gg; p[2] = bb; p[3] = 255;
                }
            }
            mSkyFaceTextures[0] = mTarget->createTexture(W, H, px.data(), true);
            mTarget->setSky(mSkyFaceTextures[0] ? SkyMode::Equirectangular : SkyMode::NoSky, mSkyFaceTextures[0]);
            if (mSkyFaceTextures[0]) {
                QImage strip(W, H, QImage::Format_RGBA8888);
                for (int r = 0; r < H; ++r)
                    std::memcpy(strip.scanLine(r), &px[size_t(r) * W * 4u], size_t(W) * 4u);
                applySkyReflection(strip);
            }
        } else if (kind == SkyKind::Realistic) {
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
                mTarget->setSky(mSkyFaceTextures[0] ? SkyMode::Equirectangular : SkyMode::NoSky, mSkyFaceTextures[0]);
                if (mSkyFaceTextures[0]) applySkyReflection(baked);
            } else {
                mTarget->setSky(SkyMode::NoSky, 0);
            }
        } else {
            mTarget->setSky(SkyMode::NoSky, 0);
        }
    }
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
    // Face order is +X,-X,+Y,-Y,+Z,-Z in WORLD axes (what Scene::setSkyCubemap
    // takes; the engine converts to Ogre's left-handed cube itself). Same basis
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

void SceneMirror::applySkyReflection(const QImage &equirect)
{
    if (equirect.isNull()) return;
    // The ambient integral runs on the FULL-resolution image (it is a mean; the
    // decimation below would bias it) before anything else touches it.
    if (integrateSkyAmbientSh(equirect, mSkyAmbientSh))
        mHasSkyAmbient = true;

    const int N = 128;   // reflection cube face size; the engine mips it further
    // Box-filter the source down to ~4 texels per face texel before sampling:
    // point-sampling a 4K equirect into 128^2 faces throws away 99.9% of it.
    const QImage src = boxDownscaleTo(equirect.convertToFormat(QImage::Format_RGBA8888), N * 4);
    const int W = src.width(), H = src.height();
    if (W <= 0 || H <= 0) return;
    // Face basis in WORLD axes (+X,-X,+Y,-Y,+Z,-Z; dir = axis + right*u + up*v
    // with image row 0 at the top) — exactly what Scene::setSkyReflection takes;
    // the engine converts to its cubemap handedness. The equirect fetch below
    // uses dirToEquirect, i.e. OGRE'S sky mapping, so a reflection lines up with
    // the sky pixel the camera sees in that direction.
    static const float ax[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    static const float rt[6][3] = {{0,0,-1},{0,0,1},{1,0,0},{1,0,0},{1,0,0},{-1,0,0}};
    static const float up[6][3] = {{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
    std::vector<unsigned char> face(size_t(N) * N * 4u);
    TextureId ids[6] = { 0, 0, 0, 0, 0, 0 };
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
    if (ok && mTarget->setSkyReflection(ids)) {
        for (int i = 0; i < 6; ++i) mReflFaceTextures[i] = ids[i];
    } else {
        for (int i = 0; i < 6; ++i) if (ids[i]) mTarget->destroyTexture(ids[i]);
    }
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
    return c;
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
    view->setPip(d);
}

void SceneMirror::applyCamera(iris::CameraNodePtr camera, View *view)
{
    if (!camera || !view) return;

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
    if (mSource && mSource->isPlaying()) {
        if (auto active = mSource->getActiveCamera()) camera = active;
    }

    // A CAMERA NEVER DRAWS ITSELF (CAMERAS_SPEC phase 2b). Whatever camera is
    // driving the view is, by definition, the one whose body would sit on the
    // near plane and whose frustum lines would fan across the whole image —
    // while piloting it (phase 3), while playing through the active camera, or
    // in any preview whose viewpoint happens to be a scene node. Recorded here
    // because this function is "the ONLY way a View's camera moves" (Engine.h),
    // so it is the one place that always knows.
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

    view->setCamera(toCameraDesc(camera));
}
