// THE SCENE'S ONE LIGHT LIST (P1C-LIGHT-LIST) — the rule is in SceneLightList.h.
#include "SceneLightList.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "EnginePrivate.h"
#include "OgreLight.h"
#include "OgreMovableObject.h"
#include "Math/Array/OgreObjectMemoryManager.h"
#include "OgreSceneManager.h"
#include "OgreSceneNode.h"
#include "OgreMath.h"

namespace jahshaka {
namespace engine {

namespace {
float peakPower(const Ogre::Light *l) {
    const Ogre::ColourValue d = l->getDiffuseColour() * l->getPowerScale();
    return std::max(std::max(d.r, d.g), d.b);
}
bool isPointSpot(Ogre::Light::LightTypes t) {
    return t == Ogre::Light::LT_POINT || t == Ogre::Light::LT_SPOTLIGHT;
}
bool isArea(Ogre::Light::LightTypes t) {
    return t == Ogre::Light::LT_AREA_APPROX || t == Ogre::Light::LT_AREA_LTC;
}
}   // namespace

namespace {
const Ogre::String &documentIdKey() {
    static const Ogre::String kKey("jahshaka.node");
    return kKey;
}
}   // namespace

void SceneLightList::bindDocumentId(Ogre::Light *l, unsigned long long id) {
    if (l) l->getUserObjectBindings().setUserAny(documentIdKey(), Ogre::Any(id));
}

unsigned long long SceneLightList::documentId(const Ogre::Light *l) {
    // getUserObjectBindings is not const in Ogre; the read changes nothing.
    const Ogre::Any &a = const_cast<Ogre::Light *>(l)->getUserObjectBindings().getUserAny(documentIdKey());
    const unsigned long long *p = a.has_value() ? Ogre::any_cast<unsigned long long>(&a) : nullptr;
    return p ? *p : 0ull;
}

float SceneLightList::orderKey(const Ogre::Light *l) {
    if (l->getType() == Ogre::Light::LT_DIRECTIONAL) return std::numeric_limits<float>::infinity();
    const float range = float(l->getAttenuationRange());
    return peakPower(l) * range * range;
}

void SceneLightList::refresh(Ogre::SceneManager *sm) const {
    const bool dirt = mSerial && mRemoved;
    if (mValid && sm == mSm && dirt && *mSerial == mBuiltSerial && *mRemoved == mBuiltRemoved) return;
    mEntries.clear();
    mByRank.clear();
    mSm = sm;
    if (dirt) {
        mBuiltSerial = *mSerial;
        mBuiltRemoved = *mRemoved;
    }
    mValid = dirt;
    ++mRebuilds;
    if (!sm) return;
    // THE LIGHT MEMORY MANAGER'S ORDER — the walk the voxel injection always made
    // (render queue by render queue, object by object), so a consumer whose lights
    // all fit its budget packs them in the order it always did.
    Ogre::ObjectMemoryManager &memory = sm->_getLightMemoryManager();
    const size_t numQueues = memory.getNumRenderQueues();
    for (size_t q = 0; q < numQueues; ++q) {
        Ogre::ObjectData objData;
        const size_t total = memory.getFirstObjectData(objData, q);
        for (size_t j = 0; j < total; j += ARRAY_PACKED_REALS) {
            for (size_t k = 0; k < ARRAY_PACKED_REALS; ++k) {
                // A pack's tail lanes past `total` hold no object.
                if (j + k >= total) break;
                const Ogre::uint32 flags = objData.mVisibilityFlags[k];
                if (!(flags & Ogre::VisibilityFlags::LAYER_VISIBILITY)) continue;
                Ogre::Light *light = static_cast<Ogre::Light *>(objData.mOwner[k]);
                if (!light || !light->getParentNode()) continue;
                mEntries.push_back({ light });
            }
            objData.advancePack();
        }
    }
    // THE ORDER (PHOTON-7 stage 1): peak intensity x range^2 - a directional light first, at +inf - and its TIES BY
    // THE WORLD (stage 1's read F3): the world position (x, y, z), then the type, the peak, the range, the colour, the
    // world direction, and last the document's persistent id (bound on the light by OgreScene) - never Ogre's movable
    // id, which is creation order: a deleted-and-undone light, or a reload in another node order, flipped an exact tie
    // and with it the first-K pick and the sum's order. A function of the lights alone; two lights equal in all of it
    // are the same light twice, and their order changes nothing.
    std::vector<std::uint32_t> order(mEntries.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [this](std::uint32_t a, std::uint32_t b) {
        const Ogre::Light *la = mEntries[a].light, *lb = mEntries[b].light;
        const float ka = orderKey(la), kb = orderKey(lb);
        if (ka != kb) return ka > kb;
        const Ogre::Vector3 xa = la->getParentNode()->_getDerivedPosition(), xb = lb->getParentNode()->_getDerivedPosition();
        for (int k = 0; k < 3; ++k)
            if (xa[k] != xb[k]) return xa[k] < xb[k];
        if (la->getType() != lb->getType()) return la->getType() < lb->getType();
        const float pa = peakPower(la), pb = peakPower(lb);
        if (pa != pb) return pa > pb;
        const float ra = float(la->getAttenuationRange()), rb = float(lb->getAttenuationRange());
        if (ra != rb) return ra < rb;
        const Ogre::ColourValue ca = la->getDiffuseColour(), cb = lb->getDiffuseColour();
        for (int k = 0; k < 3; ++k)
            if (ca[k] != cb[k]) return ca[k] < cb[k];
        const Ogre::Vector3 da = la->getDerivedDirection(), db = lb->getDerivedDirection();
        for (int k = 0; k < 3; ++k)
            if (da[k] != db[k]) return da[k] < db[k];
        return documentId(la) < documentId(lb);
    });
    mByRank.reserve(order.size());
    for (std::uint32_t r = 0; r < order.size(); ++r) mByRank.push_back(mEntries[order[r]].light);
}

const std::vector<SceneLightList::Entry> &SceneLightList::entries(Ogre::SceneManager *sm) const {
    refresh(sm);
    return mEntries;
}

const std::vector<Ogre::Light *> &SceneLightList::byRank(Ogre::SceneManager *sm) const {
    refresh(sm);
    return mByRank;
}

bool SceneLightList::reaches(const Ogre::Light *l, const Consumer &c) {
    const Ogre::Light::LightTypes t = l->getType();
    if (t == Ogre::Light::LT_DIRECTIONAL) return true;
    const bool hasBox = c.boxMin.x <= c.boxMax.x && c.boxMin.y <= c.boxMax.y && c.boxMin.z <= c.boxMax.z;
    if (!hasBox) return true;
    const Ogre::Vector3 p = l->getParentNode()->_getDerivedPosition();
    Ogre::Vector3 closest = p;
    closest.makeCeil(c.boxMin);
    closest.makeFloor(c.boxMax);
    const float d = closest.distance(p);
    const float range = float(l->getAttenuationRange());
    // THE RANGE CULL (IMAGE-1's, every consumer's): past its range a light lights nothing (the window is 0), so
    // missing the box drops nothing.
    if (!(d <= range)) return false;
    // THE CONE CULL (a spot), conservative: a spot whose apex is outside the box and whose cone - widened by the box's
    // angular radius seen from the apex - holds none of the box's eight corners lights nothing in it.
    if (t == Ogre::Light::LT_SPOTLIGHT && d > 0.0f) {
        const Ogre::Vector3 centre = (c.boxMin + c.boxMax) * 0.5f;
        const float boxRadius = float((c.boxMax - c.boxMin).length()) * 0.5f;
        const float toCentre = float(centre.distance(p));
        const float angRadius = toCentre > boxRadius ? std::asin(boxRadius / toCentre) : float(Ogre::Math::HALF_PI);
        const float halfAngle = float(l->getSpotlightOuterAngle().valueRadians()) * 0.5f;
        const float limit = halfAngle + angRadius;
        if (limit < float(Ogre::Math::PI)) {
            const float cosLimit = std::cos(limit);
            const Ogre::Vector3 dir = l->getDerivedDirection().normalisedCopy();
            for (int k = 0; k < 8; ++k) {
                const Ogre::Vector3 corner((k & 1) ? c.boxMax.x : c.boxMin.x, (k & 2) ? c.boxMax.y : c.boxMin.y,
                                           (k & 4) ? c.boxMax.z : c.boxMin.z);
                Ogre::Vector3 v = corner - p;
                const float len = float(v.length());
                if (len <= 0.0f || float(v.dotProduct(dir)) >= cosLimit * len) return true;
            }
            return false;
        }
    }
    return true;
}

void SceneLightList::select(Ogre::SceneManager *sm, const Consumer &c, Selection &out) const {
    out.lights.clear();
    out.inRange = 0u;
    out.overBudget = 0u;
    refresh(sm);
    const Ogre::uint32 mask = c.lightMask & Ogre::VisibilityFlags::RESERVED_VISIBILITY_FLAGS;
    // IN THE ORDER, the first `budget` that reach the box - the rest counted.
    for (Ogre::Light *l : mByRank) {
        // Visibility is read live as well (a hide bumps the dirt; this is the guard).
        if (!l->getVisible() || !(l->getVisibilityFlags() & mask)) continue;
        const Ogre::Light::LightTypes t = l->getType();
        const bool wanted = (t == Ogre::Light::LT_DIRECTIONAL && c.directional) ||
                            (isPointSpot(t) && c.pointSpot) || (isArea(t) && c.area);
        if (!wanted || !reaches(l, c)) continue;
        ++out.inRange;
        if (c.budget && out.lights.size() >= c.budget) continue;
        out.lights.push_back(l);
    }
    out.overBudget = out.inRange - std::uint32_t(out.lights.size());
}

const SceneLightList &sceneLightListOf(Ogre::SceneManager *sm) {
    const detail::SceneGiBinding *b = detail::sceneGiBindingOf(sm);
    if (b && b->lightList) return *b->lightList;
    // No scene record: one list with no dirt, rebuilt on every read.
    static SceneLightList sUnbound;
    return sUnbound;
}

}   // namespace engine
}   // namespace jahshaka
