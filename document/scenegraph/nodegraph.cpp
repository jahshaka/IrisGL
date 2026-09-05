/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

// THE document library's one Ogre translation unit (SPECS/SCENEGRAPH_SPEC.md).
// Nothing above it may include an Ogre header; see nodegraph.h for the law and
// the list of sanctioned TUs.

#include "document/scenegraph/nodegraph.h"
#include "document/scenegraph/scenenode.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include <QDebug>

#include "OgreMatrix4.h"
#include "OgreMovableObject.h"
#include "OgreQuaternion.h"
#include "OgreRay.h"
#include "OgreRoot.h"
#include "OgreHlmsManager.h"
#include "OgreSceneManager.h"
#include "OgreSceneNode.h"
#include "OgreSceneQuery.h"
#include "OgreVector3.h"
#include "Math/Array/OgreObjectMemoryManager.h"

namespace iris
{
namespace graph
{

namespace
{

/// IS THERE STILL AN ENGINE? Ogre::Root is a Singleton whose base destructor
/// clears the instance pointer, so this answers false the moment the engine is
/// gone — and every handle in the program dangles at that instant, because the
/// nodes were the Root's scene managers' nodes.
///
/// The document outlives the engine by design at shutdown (MainWindow destroys
/// the Engine at step 5 and its scene later), so every entry point here is
/// guarded by this rather than by an ordering rule nobody can enforce. It costs
/// one load of a static pointer.
inline bool engineAlive() { return Ogre::Root::getSingletonPtr() != nullptr; }

inline Ogre::SceneManager *sm(SceneHandle h) { return reinterpret_cast<Ogre::SceneManager *>(h); }
inline SceneHandle wrap(Ogre::SceneManager *s) { return reinterpret_cast<SceneHandle>(s); }
inline Ogre::SceneNode *nd(NodeHandle h) { return reinterpret_cast<Ogre::SceneNode *>(h); }
inline NodeHandle wrap(Ogre::SceneNode *n) { return reinterpret_cast<NodeHandle>(n); }

inline Ogre::Vector3 toOgre(const Vec3 &v) { return Ogre::Vector3(v.x(), v.y(), v.z()); }
inline Vec3 toIris(const Ogre::Vector3 &v) { return Vec3(v.x, v.y, v.z); }
inline Ogre::Quaternion toOgre(const Quat &q)
{
    return Ogre::Quaternion(q.scalar(), q.x(), q.y(), q.z());
}
inline Quat toIris(const Ogre::Quaternion &q) { return Quat(q.w, q.x, q.y, q.z); }

/// Ogre::Matrix4 is row-major (m[row][col]); iris::Mat4's 16-argument
/// constructor takes its arguments in the same row-major reading order (it is
/// QMatrix4x4's constructor, transition for transition — core/math/mat4.h).
inline Mat4 toIris(const Ogre::Matrix4 &m)
{
    return Mat4(m[0][0], m[0][1], m[0][2], m[0][3],
                m[1][0], m[1][1], m[1][2], m[1][3],
                m[2][0], m[2][1], m[2][2], m[2][3],
                m[3][0], m[3][1], m[3][2], m[3][3]);
}

// ---------------------------------------------------------------------------
// THE BACK-POINTER TABLE — Ogre node id -> document handle.
//
// The obvious home for this is Ogre's UserObjectBindings, and that is where it
// started. It cost too much: `setUserAny` allocates an Attributes block on
// demand and `Ogre::Any`'s copy constructor CLONES its holder, so a back-
// pointer was three heap allocations on the path every document node creation
// takes. Measured on the §6 benchmark: 1.3 us per node, +23% on e.build_doc,
// i.e. the single largest cost the whole swap added. Removing it puts document
// build back at pre-swap parity.
//
// So: a chunked flat array indexed by `Ogre::Node::getId()` (a process-wide
// monotonic counter — OgreSceneManager.cpp:915). Lookup is two loads and no
// hashing, which matters because ownerOf() is on the hot path of every
// children() and every getParent(). Writes take the graph mutex; reads take
// nothing, and are safe against a concurrent write because the chunk POINTERS
// are atomic and a chunk, once published, is never moved or freed.
//
// Cost: 512 KB of untouched BSS for the chunk directory, and 64 KB per 8192
// live ids actually used. Ids are never reused, so a session that creates
// hundreds of millions of nodes would run off the end — that is reported once
// and then the node simply has no owner (it reads as engine-owned), which
// degrades to "invisible to the document" rather than to a wrong answer.
constexpr std::size_t kOwnerChunkShift = 13;                     // 8192 ids / chunk
constexpr std::size_t kOwnerChunkSize = std::size_t(1) << kOwnerChunkShift;
constexpr std::size_t kOwnerChunkCount = 65536;                  // 536 M ids
std::atomic<SceneNode **> gOwnerChunks[kOwnerChunkCount];

inline SceneNode *ownerById(Ogre::IdType id)
{
    const std::size_t chunk = std::size_t(id) >> kOwnerChunkShift;
    if (chunk >= kOwnerChunkCount) return nullptr;
    SceneNode **entries = gOwnerChunks[chunk].load(std::memory_order_acquire);
    return entries ? entries[std::size_t(id) & (kOwnerChunkSize - 1)] : nullptr;
}

/// Under graphMutex().
void setOwnerById(Ogre::IdType id, SceneNode *owner)
{
    const std::size_t chunk = std::size_t(id) >> kOwnerChunkShift;
    if (chunk >= kOwnerChunkCount) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning("iris::graph: the scene-node id space is exhausted (id %llu) — document "
                     "nodes created from here on have no back-pointer.",
                     static_cast<unsigned long long>(id));
        }
        return;
    }
    SceneNode **entries = gOwnerChunks[chunk].load(std::memory_order_relaxed);
    if (!entries) {
        entries = new SceneNode *[kOwnerChunkSize]();
        gOwnerChunks[chunk].store(entries, std::memory_order_release);
    }
    entries[std::size_t(id) & (kOwnerChunkSize - 1)] = owner;
}

std::recursive_mutex &graphMutex()
{
    static std::recursive_mutex m;
    return m;
}

/// The staging scene manager and the Root it was made from. The Root pointer is
/// how a stale cache is spotted after an engine restart inside one process
/// (the suites do that): a different Root means our SceneManager died with the
/// old one.
Ogre::Root *gStagingRoot = nullptr;
Ogre::SceneManager *gStaging = nullptr;
/// Did WE create it (the fallback path)? A host-supplied one belongs to the
/// engine and must not be destroyed here.
bool gStagingIsOurs = false;
std::size_t gLiveNodes = 0;
/// How many live handles sit in a SCENE_STATIC memory manager. Maintained by
/// applyStaticSubtree and by the destroy/migrate paths, so the benchmark can
/// assert that its 80% static population really reached the static manager
/// rather than reporting a hint nobody applied.
std::size_t gStaticNodes = 0;

Ogre::SceneNode *rootOf(Ogre::SceneManager *s)
{
    return s->getRootSceneNode(Ogre::SCENE_DYNAMIC);
}

inline SceneNode *ownerRaw(Ogre::SceneNode *n) { return ownerById(n->getId()); }

// ---- SCENE_STATIC ---------------------------------------------------------

/// Rule 2 (nodegraph.h): a static node's parent must be static, or must be the
/// DOCUMENT ROOT. The document root is recognised structurally — it is the
/// document node whose own Ogre parent is the scene manager's root node — and
/// its local transform is identity for the life of the scene (nothing in
/// either repo writes a transform on `Scene::getRootNode()`; SceneNode's
/// transform setters say so and Scene::rootNode is never handed to a command).
bool isDocumentRoot(Ogre::Node *n)
{
    // A document node with no document node above it: either its Ogre parent is
    // the scene manager's own root node (a scene bound to an engine, and the
    // node detach() re-homes things under), or it has NO parent at all — which
    // is how every node is born and how `Scene::Scene` builds its root before
    // the document has ever met an engine scene.
    Ogre::Node *p = n->getParent();
    return !p || ownerById(p->getId()) == nullptr;
}

bool eligibleStaticParent(Ogre::Node *p)
{
    if (!p) return false;                       // detached: nothing anchors it
    if (p->isStatic()) return true;
    return isDocumentRoot(p);
}

/// Switches ONE node, attachments included, without SceneNode::setStatic's
/// half-way failure mode: that one migrates the node FIRST and only then
/// throws if an attachment refuses, leaving the node in the new manager and
/// the object in the old — which is the state `attachObject` calls a bug.
/// Returns false having changed nothing.
bool switchOne(Ogre::SceneNode *n, bool value)
{
    if (n->isStatic() == value) return true;
    // Attachments first. MovableObject::setStatic REPORTS a refusal (its
    // object memory manager has no twin — lights and PFX2 particle systems)
    // instead of throwing, and it drags the parent node along with it, so
    // after the first success the node is already in the right manager.
    std::vector<Ogre::MovableObject *> switched;
    switched.reserve(n->numAttachedObjects());
    for (std::size_t i = 0; i < n->numAttachedObjects(); ++i) {
        Ogre::MovableObject *obj = n->getAttachedObject(i);
        if (obj->isStatic() == value) continue;
        if (!obj->setStatic(value)) {
            for (Ogre::MovableObject *done : switched) done->setStatic(!value);
            return false;
        }
        switched.push_back(obj);
    }
    // Node::setStatic, not SceneNode::setStatic: the attachments are already
    // where they belong, and the override would walk them a second time.
    n->Ogre::Node::setStatic(value);
    if (value && n->getCreator()) n->getCreator()->notifyStaticDirty(n);
    return true;
}

/// May THIS node go static? An engine-owned node (a light's -Y adapter, a
/// decal box, a wire) has no opinion and rides its parent; a document node
/// answers with its own kind (SceneNode::isStaticEligible).
inline bool eligibleForStatic(Ogre::SceneNode *n)
{
    SceneNode *o = ownerById(n->getId());
    return !o || o->isStaticEligible();
}

/// Rule 1: static is a SUBTREE property.
///
/// SKIP, DO NOT VETO. A branch that cannot go static — a light inside an
/// imported model, an animated node, a particle emitter — is left dynamic
/// together with everything under it, and the REST of the subtree still goes
/// static. (Ogre is explicit that a dynamic child of a static parent is valid
/// and useful; it is the opposite that is a bug.) Vetoing instead would mean
/// one lamp in a building denied the whole building its classification.
///
/// Returns whether the ROOT `n` itself changed class — that is the only
/// refusal a caller can act on. Demotion never fails: the only objects that
/// cannot switch (lights, particle systems) are permanently dynamic anyway, so
/// they are never in a state a demotion has to undo.
bool applyStaticSubtree(Ogre::SceneNode *n, bool value)
{
    std::function<bool(Ogre::SceneNode *)> walk = [&](Ogre::SceneNode *cur) -> bool {
        if (value && !eligibleForStatic(cur)) return false;   // skip it and its branch
        const bool was = cur->isStatic();
        if (!switchOne(cur, value)) return false;
        if (was != cur->isStatic()) {
            if (value) ++gStaticNodes;
            else if (gStaticNodes) --gStaticNodes;
        }
        for (std::size_t i = 0; i < cur->numChildren(); ++i)
            walk(static_cast<Ogre::SceneNode *>(cur->getChild(i)));
        return true;
    };
    return walk(n);
}

/// The document's own answer to "should this subtree be static?", read through
/// the owner back-pointer. An engine-owned node has no opinion.
inline bool ownerWantsStatic(Ogre::SceneNode *n)
{
    SceneNode *owner = ownerById(n->getId());
    return owner && owner->wantsStatic();
}

/// Brings a freshly (re-)parented subtree's class in line with the document's
/// hint. Called once per structural move, on the SUBTREE ROOT.
///
/// The default case costs NOTHING: a node with no static hint attached to a
/// dynamic parent is already dynamic (it inherited the parent's manager), so
/// the comparison fails and no migration happens. Only a static-hinted subtree
/// under an eligible parent, or a dynamic subtree that inherited static from a
/// static parent, pays.
void reconcileStatic(Ogre::SceneNode *n)
{
    const bool want = ownerWantsStatic(n) && eligibleStaticParent(n->getParent());
    if (n->isStatic() == want) return;
    applyStaticSubtree(n, want);
}

/// Depth-first, deepest-first destruction. Ogre's destroySceneNode ORPHANS the
/// children it does not destroy and leaks them into mSceneNodes until
/// clearScene (spec §5, probe-verified) — this is the one path that does not.
///
/// A child with NO document owner belongs to the ENGINE (a light's -Y adapter,
/// a decal's projector box, a light's range wire). Those are not ours to
/// destroy — the engine's releaseNode still holds raw pointers to them — so
/// they are re-homed under their scene's root and left for it to clean up.
void destroyRecursive(Ogre::SceneNode *n)
{
    Ogre::SceneNode *sceneRoot = rootOf(n->getCreator());
    while (n->numChildren() > 0) {
        Ogre::SceneNode *c = static_cast<Ogre::SceneNode *>(n->getChild(0));
        if (ownerRaw(c)) {
            destroyRecursive(c);
        } else {
            n->removeChild(c);
            if (c != sceneRoot) sceneRoot->addChild(c);
        }
    }
    if (SceneNode *owner = ownerRaw(n)) owner->_setGraphNode(nullptr);
    setOwnerById(n->getId(), nullptr);
    if (gLiveNodes) --gLiveNodes;
    if (n->isStatic() && gStaticNodes) --gStaticNodes;
    if (n->getParent()) n->getParent()->removeChild(n);
    n->getCreator()->destroySceneNode(n);
}

}  // namespace

bool available() { return Ogre::Root::getSingletonPtr() != nullptr; }

void setStagingScene(SceneHandle s)
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    gStaging = sm(s);
    gStagingRoot = Ogre::Root::getSingletonPtr();
    gStagingIsOurs = false;
}

SceneHandle stagingScene()
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) return nullptr;
    if (gStaging && gStagingRoot == root) return wrap(gStaging);
    // A new Root: whatever we cached died with the old one.
    gStaging = nullptr;
    gStagingIsOurs = false;
    gStagingRoot = root;
    // FALLBACK, for hosts that never called setStagingScene (the suites, which
    // always create their View before their first document node). A scene
    // manager cannot be constructed before the backend has a Window and its
    // resource groups initialised — its constructor looks up a material the
    // common scripts define and dereferences the result — and "the Hlms is
    // registered" is the one test for that state visible from here. Answer null
    // rather than crash inside Ogre: the caller reports it.
    if (!root->getHlmsManager() || !root->getHlmsManager()->getHlms(Ogre::HLMS_PBS))
        return nullptr;
    gStaging = root->createSceneManager(Ogre::ST_GENERIC, 1u, "iris-staging");
    gStagingIsOurs = true;
    return wrap(gStaging);
}

SceneHandle sceneOf(NodeHandle n)
{
    if (!n || !engineAlive()) return nullptr;
    return wrap(nd(n)->getCreator());
}

bool sceneAlive(SceneHandle s)
{
    if (!s) return false;
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) return false;
    Ogre::SceneManagerEnumerator::SceneManagerIterator it = root->getSceneManagerIterator();
    for (auto cur = it.begin(); cur != it.end(); ++cur)
        if (cur->second == sm(s)) return true;
    return false;
}

void shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    if (gStagingIsOurs && gStaging && Ogre::Root::getSingletonPtr() == gStagingRoot && gStagingRoot)
        gStagingRoot->destroySceneManager(gStaging);
    gStaging = nullptr;
    gStagingRoot = nullptr;
    gStagingIsOurs = false;
}

NodeHandle createNode(SceneHandle s, NodeHandle parent, SceneNode *owner)
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    if (!s || !engineAlive()) return nullptr;
    // PARENTLESS when no parent is named. A node created under the scene's root
    // and then re-parented by insertChild pays TWO NodeMemoryManager migrations
    // (Node::setParent detaches from one depth level and attaches to another),
    // and "create it, set it up, then add it to something" is how every caller
    // in the tree builds a document. Ogre-Next supports a parentless node — it
    // sits at depth 0 against the manager's dummy parent transform.
    Ogre::SceneNode *n = parent ? nd(parent)->createChildSceneNode(Ogre::SCENE_DYNAMIC)
                                : sm(s)->createSceneNode(Ogre::SCENE_DYNAMIC);
    setOwnerById(n->getId(), owner);
    ++gLiveNodes;
    return wrap(n);
}

void destroyNode(NodeHandle n)
{
    if (!n || !engineAlive()) return;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    destroyRecursive(nd(n));
}

SceneNode *ownerOf(NodeHandle n) { return (n && engineAlive()) ? ownerRaw(nd(n)) : nullptr; }

NodeHandle parentOf(NodeHandle n)
{
    if (!n || !engineAlive()) return nullptr;
    Ogre::Node *p = nd(n)->getParent();
    return p ? wrap(static_cast<Ogre::SceneNode *>(p)) : nullptr;
}

std::size_t childCount(NodeHandle n) { return (n && engineAlive()) ? nd(n)->numChildren() : 0; }

NodeHandle childAt(NodeHandle n, std::size_t i)
{
    if (!n || !engineAlive() || i >= nd(n)->numChildren()) return nullptr;
    return wrap(static_cast<Ogre::SceneNode *>(nd(n)->getChild(i)));
}

int indexInParent(NodeHandle n)
{
    if (!n || !engineAlive()) return -1;
    Ogre::Node *p = nd(n)->getParent();
    if (!p) return -1;
    // The DOCUMENT sibling index — engine-owned children (a light's -Y adapter,
    // a decal box, a range wire) share the vector and must not be counted.
    // This is the inverse of attach()'s `index`, which resolves against owned
    // children too; a raw vector index here would have made the pair disagree
    // by however many helpers the engine had hung off that parent, silently
    // reordering the hierarchy panel on every undo of a delete.
    int owned = 0;
    for (std::size_t i = 0; i < p->numChildren(); ++i) {
        Ogre::SceneNode *c = static_cast<Ogre::SceneNode *>(p->getChild(i));
        if (c == nd(n)) return owned;
        if (ownerRaw(c)) ++owned;
    }
    return -1;
}

void attach(NodeHandle parent, NodeHandle child, int index)
{
    if (!parent || !child || !engineAlive()) return;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::SceneNode *p = nd(parent);
    Ogre::SceneNode *c = nd(child);
    if (c->getParent()) c->getParent()->removeChild(c);
    // APPEND is the overwhelming majority (addChild passes -1) and must not pay
    // for the sibling-index machinery below: at a fan-out of k that scan is
    // O(k) per insert, i.e. quadratic in a node's child count over a whole
    // document build.
    if (index < 0 || std::size_t(index) >= p->numChildren()) {
        p->addChild(c);
        reconcileStatic(c);
        return;
    }

    // Ogre only appends, and its child vector also carries the ENGINE's own
    // children (a light's -Y adapter, a decal box, a range wire), which the
    // document cannot see. `index` is a DOCUMENT sibling index, so it is
    // resolved against owned children only — and the sibling index is real
    // semantics (the hierarchy panel shows it, DeleteSceneNodeCommand captures
    // it for undo — audit item 3).
    std::vector<Ogre::Node *> kids;
    kids.reserve(p->numChildren());
    for (std::size_t i = 0; i < p->numChildren(); ++i) kids.push_back(p->getChild(i));

    std::size_t insertAt = kids.size();
    int owned = 0;
    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (!ownerRaw(static_cast<Ogre::SceneNode *>(kids[i]))) continue;
        if (owned == index) { insertAt = i; break; }
        ++owned;
    }
    if (insertAt >= kids.size()) { p->addChild(c); reconcileStatic(c); return; }

    for (Ogre::Node *k : kids) p->removeChild(k);
    kids.insert(kids.begin() + std::ptrdiff_t(insertAt), c);
    for (Ogre::Node *k : kids) p->addChild(k);
    reconcileStatic(c);
}

NodeHandle detach(NodeHandle child)
{
    if (!child) return nullptr;
    if (!engineAlive()) return child;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::SceneNode *c = nd(child);
    // Out of its parent and under its scene manager's root — NOT migrated to
    // the staging manager, which is what this used to do. A migration rebuilds
    // the whole subtree, which changes every handle in it, which makes the
    // mirror release and re-adopt every one of those nodes (and re-attach every
    // mesh and material) on the next sync. Measured on the §6 benchmark, that
    // turned a 500-node reparent — remove from one parent, add to another —
    // into two full subtree rebuilds plus 1000 re-adoptions, ~10% of
    // c.reparent_500 at 1k.
    //
    // What the migration bought was safety against "the engine scene is
    // destroyed while the undo stack still holds a node that was in it". That
    // is now iris::Scene's job instead: it remembers the subtrees detached from
    // it (Scene::rememberDetached) and takes them along when it unbinds.
    if (c->getParent()) c->getParent()->removeChild(c);
    Ogre::SceneNode *root = rootOf(c->getCreator());
    if (c != root) root->addChild(c);
    return child;
}

NodeHandle migrate(NodeHandle n, SceneHandle target, NodeHandle newParent)
{
    if (!n || !target) return nullptr;
    if (!engineAlive()) return n;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::SceneNode *src = nd(n);
    if (src->getCreator() == sm(target) && !newParent) return n;

    // Rebuild, do not move: an Ogre::SceneNode belongs to the SceneManager that
    // created it (mCreator decides who may destroy it and who creates its
    // children), and Node::setParent's cross-memory-manager migration moves
    // only the transform storage. Rebuilding keeps ONE owner per node.
    std::function<Ogre::SceneNode *(Ogre::SceneNode *, Ogre::SceneNode *)> copy =
        [&](Ogre::SceneNode *s, Ogre::SceneNode *parent) -> Ogre::SceneNode * {
        Ogre::SceneNode *dst = parent->createChildSceneNode(Ogre::SCENE_DYNAMIC);
        dst->setPosition(s->getPosition());
        dst->setOrientation(s->getOrientation());
        dst->setScale(s->getScale());
        SceneNode *owner = ownerRaw(s);
        if (owner) {
            setOwnerById(dst->getId(), owner);
            owner->_setGraphNode(wrap(dst));
        }
        // Visibility is NOT copied: Ogre's setVisible walks a node's
        // ATTACHMENTS, and a document node in the staging manager has none. The
        // handle's own `visible` flag is the truth and the mirror re-applies it.
        ++gLiveNodes;
        // Engine-owned children (no document owner) are NOT copied: they belong
        // to the scene manager that is being left behind, and the engine drops
        // them on its own schedule.
        for (std::size_t i = 0; i < s->numChildren(); ++i) {
            Ogre::SceneNode *c = static_cast<Ogre::SceneNode *>(s->getChild(i));
            if (ownerRaw(c)) copy(c, dst);
        }
        // Static-ness is NOT carried here: `dst` was created under `parent`
        // and has already inherited its class (Node::setParent gives a child
        // its parent's memory manager). The caller reconciles the SUBTREE ROOT
        // with the document's hint once the whole copy exists — doing it per
        // node inside the recursion would migrate the same node twice.
        return dst;
    };

    Ogre::SceneNode *parent = newParent ? nd(newParent) : rootOf(sm(target));
    Ogre::SceneNode *dst = copy(src, parent);
    // The source subtree's owners have already been re-pointed at the copies,
    // so this drop must NOT clear them again — hence its own walk rather than
    // destroyRecursive. Engine-owned children are re-homed, never destroyed.
    std::function<void(Ogre::SceneNode *)> drop = [&](Ogre::SceneNode *s) {
        Ogre::SceneNode *sceneRoot = rootOf(s->getCreator());
        while (s->numChildren() > 0) {
            Ogre::SceneNode *c = static_cast<Ogre::SceneNode *>(s->getChild(0));
            if (ownerRaw(c)) {
                drop(c);
            } else {
                s->removeChild(c);
                if (c != sceneRoot) sceneRoot->addChild(c);
            }
        }
        setOwnerById(s->getId(), nullptr);
        if (gLiveNodes) --gLiveNodes;
        if (s->isStatic() && gStaticNodes) --gStaticNodes;
        if (s->getParent()) s->getParent()->removeChild(s);
        s->getCreator()->destroySceneNode(s);
    };
    drop(src);
    reconcileStatic(dst);
    return wrap(dst);
}

// ---- transforms -----------------------------------------------------------

Vec3 localPos(NodeHandle n) { return (n && engineAlive()) ? toIris(nd(n)->getPosition()) : Vec3(); }
Quat localRot(NodeHandle n) { return (n && engineAlive()) ? toIris(nd(n)->getOrientation()) : Quat(); }
Vec3 localScale(NodeHandle n) { return (n && engineAlive()) ? toIris(nd(n)->getScale()) : Vec3(1, 1, 1); }

namespace
{
/// RULE 4 (nodegraph.h): MOVING A STATIC NODE PROMOTES IT.
///
/// The alternative — notifyStaticDirty on every write, which is what v1 did —
/// re-runs the whole static transform+bounds pass from that node's depth on
/// the next frame. For a node the user is DRAGGING that is one full static
/// pass per frame, i.e. exactly the cost SCENE_STATIC exists to avoid, paid
/// while the scene is least able to afford it. Demoting the node (and its
/// subtree, rule 1) makes the second write and every one after it free, and
/// the document's hint is cleared with it so nothing puts it back.
///
/// Out of line and behind an `isStatic()` test that is false for almost every
/// node in almost every scene: this sits on the path of every transform write
/// in the program.
void promoteOnWrite(Ogre::SceneNode *n);
void promoteStaticChildren(Ogre::SceneNode *n);

inline void markMoved(Ogre::SceneNode *n)
{
    if (n->isStatic()) { promoteOnWrite(n); return; }
    // The root-moved case (see promoteStaticChildren). Two loads, and only in a
    // process that has static nodes at all.
    if (gStaticNodes && isDocumentRoot(n)) promoteStaticChildren(n);
}

void promoteOnWrite(Ogre::SceneNode *n)
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    if (SceneNode *owner = ownerById(n->getId())) owner->_clearStaticHint();
    applyStaticSubtree(n, false);
}

/// THE ONE HOLE RULE 2 LEAVES, closed. A static node's parent must be static
/// OR be the document root, and the root is exempt because its transform is
/// identity and nothing writes it. "Nothing writes it" is a claim, not a
/// mechanism — so if something DOES write it, every static node under it would
/// freeze at the root's old world position. This promotes them instead.
///
/// It runs only for a node that is itself a document root (two loads to tell)
/// in a process that has static nodes at all (one load), so the ordinary
/// transform write — a child, in a scene with or without statics — pays a
/// predictable compare and nothing else.
void promoteStaticChildren(Ogre::SceneNode *n)
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    bool any = false;
    for (std::size_t i = 0; i < n->numChildren(); ++i) {
        Ogre::SceneNode *c = static_cast<Ogre::SceneNode *>(n->getChild(i));
        if (!c->isStatic()) continue;
        if (SceneNode *owner = ownerById(c->getId())) owner->_clearStaticHint();
        applyStaticSubtree(c, false);
        any = true;
    }
    if (any)
        qWarning("iris::graph: the scene's ROOT node was moved — every static subtree under it "
                 "has been promoted back to dynamic, because a static node cannot follow a "
                 "moving parent (SCENEGRAPH_SPEC §6 rule 2).");
}
}  // namespace

void setLocalPos(NodeHandle n, const Vec3 &v)
{
    if (!n || !engineAlive()) return;
    nd(n)->setPosition(toOgre(v));
    markMoved(nd(n));
}

void setLocalRot(NodeHandle n, const Quat &q)
{
    if (!n || !engineAlive()) return;
    nd(n)->setOrientation(toOgre(q));
    markMoved(nd(n));
}

void setLocalScale(NodeHandle n, const Vec3 &v)
{
    if (!n || !engineAlive()) return;
    nd(n)->setScale(toOgre(v));
    markMoved(nd(n));
}

void setLocalTrs(NodeHandle n, const Vec3 &p, const Quat &r, const Vec3 &s)
{
    if (!n || !engineAlive()) return;
    Ogre::SceneNode *o = nd(n);
    o->setPosition(toOgre(p));
    o->setOrientation(toOgre(r));
    o->setScale(toOgre(s));
    markMoved(o);
}

Mat4 localTransform(NodeHandle n)
{
    if (!n || !engineAlive()) return Mat4();
    Ogre::Matrix4 m;
    m.makeTransform(nd(n)->getPosition(), nd(n)->getScale(), nd(n)->getOrientation());
    return toIris(m);
}

Mat4 globalTransform(NodeHandle n)
{
    if (!n || !engineAlive()) return Mat4();
    return toIris(nd(n)->_getFullTransformUpdated());
}

Vec3 globalPos(NodeHandle n) { return (n && engineAlive()) ? toIris(nd(n)->_getDerivedPositionUpdated()) : Vec3(); }
Quat globalRot(NodeHandle n) { return (n && engineAlive()) ? toIris(nd(n)->_getDerivedOrientationUpdated()) : Quat(); }

void setGlobalPos(NodeHandle n, const Vec3 &v)
{
    if (!n || !engineAlive()) return;
    Ogre::SceneNode *o = nd(n);
    Ogre::Node *p = o->getParent();
    if (!p) { o->setPosition(toOgre(v)); markMoved(o); return; }
    const Ogre::Matrix4 inv = p->_getFullTransformUpdated().inverseAffine();
    o->setPosition(inv * toOgre(v));
    markMoved(o);
}

void setGlobalRot(NodeHandle n, const Quat &q)
{
    if (!n || !engineAlive()) return;
    Ogre::SceneNode *o = nd(n);
    Ogre::Node *p = o->getParent();
    if (!p) { o->setOrientation(toOgre(q)); markMoved(o); return; }
    o->setOrientation(p->_getDerivedOrientationUpdated().Inverse() * toOgre(q));
    markMoved(o);
}

void setGlobalTransform(NodeHandle n, const Mat4 &m)
{
    if (!n || !engineAlive()) return;
    Ogre::SceneNode *o = nd(n);
    Ogre::Matrix4 world(m(0, 0), m(0, 1), m(0, 2), m(0, 3),
                        m(1, 0), m(1, 1), m(1, 2), m(1, 3),
                        m(2, 0), m(2, 1), m(2, 2), m(2, 3),
                        m(3, 0), m(3, 1), m(3, 2), m(3, 3));
    Ogre::Matrix4 local = world;
    if (Ogre::Node *p = o->getParent())
        local = p->_getFullTransformUpdated().inverseAffine() * world;
    Ogre::Vector3 pos, scale;
    Ogre::Quaternion rot;
    local.decomposition(pos, scale, rot);
    o->setPosition(pos);
    o->setOrientation(rot);
    o->setScale(scale);
    markMoved(o);
}

// ---- flags ----------------------------------------------------------------

void setVisible(NodeHandle n, bool visible, bool cascade)
{
    if (!n || !engineAlive()) return;
    nd(n)->setVisible(visible, cascade);
}

bool isStatic(NodeHandle n) { return n && engineAlive() && nd(n)->isStatic(); }

bool canBeStatic(NodeHandle n)
{
    if (!n || !engineAlive()) return false;
    return eligibleStaticParent(nd(n)->getParent());
}

bool setStatic(NodeHandle n, bool value)
{
    if (!n || !engineAlive()) return false;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::SceneNode *o = nd(n);
    if (o->isStatic() == value) return true;                    // already there
    if (value && !eligibleStaticParent(o->getParent())) {
        qWarning("iris::graph::setStatic(true) refused: a static node's parent must be static "
                 "or the document root (SCENEGRAPH_SPEC §6 rule 2).");
        return false;
    }
    return applyStaticSubtree(o, value);
}

std::size_t staticNodeCount() { return gStaticNodes; }

std::size_t liveNodeCount() { return gLiveNodes; }

// ---- picking: the broad phase ---------------------------------------------

bool hasQueryableGeometry(SceneHandle s)
{
    if (!s || !engineAlive()) return false;
    Ogre::SceneManager *m = sm(s);
    return m->_getEntityMemoryManager(Ogre::SCENE_DYNAMIC).getTotalNumObjects() > 0 ||
           m->_getEntityMemoryManager(Ogre::SCENE_STATIC).getTotalNumObjects() > 0;
}

namespace
{
/// Collects the DISTINCT document nodes a ray sweep hit. Ogre reports one
/// result per MovableObject and a node may carry several (a mesh Item plus a
/// wire overlay), so the listener keeps the nearest entry per node.
class OwnerCollector : public Ogre::RaySceneQueryListener
{
public:
    explicit OwnerCollector(std::vector<RayCandidate> &out) : mOut(out) {}

    bool queryResult(Ogre::MovableObject *obj, Ogre::Real distance) override
    {
        Ogre::Node *parent = obj ? obj->getParentNode() : nullptr;
        if (!parent) return true;
        SceneNode *owner = ownerById(parent->getId());
        // An engine-owned node (a light's -Y adapter, a decal box, a wire) has
        // no document owner: its hits belong to no document node and are
        // dropped rather than attributed to the nearest one.
        if (!owner) return true;
        for (RayCandidate &c : mOut) {
            if (c.node != owner) continue;
            if (float(distance) < c.distance) c.distance = float(distance);
            return true;
        }
        mOut.push_back(RayCandidate{ owner, float(distance) });
        return true;
    }

    bool queryResult(Ogre::SceneQuery::WorldFragment *, Ogre::Real) override { return true; }

private:
    std::vector<RayCandidate> &mOut;
};
}  // namespace

void rayQuery(SceneHandle s, const Vec3 &origin, const Vec3 &dir, unsigned queryMask,
              std::vector<RayCandidate> &out)
{
    out.clear();
    if (!s || !engineAlive()) return;
    Ogre::SceneManager *m = sm(s);
    // The query object is created and destroyed per call on purpose: it is a
    // small allocation, and a cached one would have to be invalidated when the
    // scene manager dies (which happens on every world switch).
    Ogre::RaySceneQuery *q = m->createRayQuery(
        Ogre::Ray(toOgre(origin), toOgre(dir).normalisedCopy()), queryMask);
    OwnerCollector collector(out);
    try {
        q->execute(&collector);
    } catch (const Ogre::Exception &e) {
        qWarning("iris::graph::rayQuery failed: %s", e.getDescription().c_str());
    }
    m->destroyQuery(q);
}

void setPickable(NodeHandle n, bool pickable)
{
    if (!n || !engineAlive()) return;
    Ogre::SceneNode *o = nd(n);
    const Ogre::uint32 flags = pickable ? kPickableQueryBit : kUnpickableQueryBit;
    for (std::size_t i = 0; i < o->numAttachedObjects(); ++i)
        o->getAttachedObject(i)->setQueryFlags(flags);
}

}  // namespace graph
}  // namespace iris
