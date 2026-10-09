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
/// JahBrdf_piece_all's window: saturate(1 - (d/R)^4)^2.
float lightWindow(float d, float range) {
    const float x = d / range;
    const float x2 = x * x;
    const float w = std::min(std::max(1.0f - x2 * x2, 0.0f), 1.0f);
    return w * w;
}
}   // namespace

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
                mEntries.push_back({ light, 0u });
            }
            objData.advancePack();
        }
    }
    // THE SHARED ORDER: brightest first, the id as the tie (the hit decode's sort).
    std::vector<std::uint32_t> order(mEntries.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [this](std::uint32_t a, std::uint32_t b) {
        const float pa = peakPower(mEntries[a].light), pb = peakPower(mEntries[b].light);
        if (pa != pb) return pa > pb;
        return mEntries[a].light->getId() < mEntries[b].light->getId();
    });
    mByRank.reserve(order.size());
    for (std::uint32_t r = 0; r < order.size(); ++r) {
        mEntries[order[r]].rank = r;
        mByRank.push_back(mEntries[order[r]].light);
    }
}

const std::vector<SceneLightList::Entry> &SceneLightList::entries(Ogre::SceneManager *sm) const {
    refresh(sm);
    return mEntries;
}

const std::vector<Ogre::Light *> &SceneLightList::byRank(Ogre::SceneManager *sm) const {
    refresh(sm);
    return mByRank;
}

float SceneLightList::contribution(const Ogre::Light *l, const Consumer &c, bool &reaches) {
    reaches = true;
    const Ogre::Light::LightTypes t = l->getType();
    const float power = peakPower(l);
    if (t == Ogre::Light::LT_DIRECTIONAL) return std::numeric_limits<float>::infinity();
    const bool hasBox = c.boxMin.x <= c.boxMax.x && c.boxMin.y <= c.boxMax.y && c.boxMin.z <= c.boxMax.z;
    if (!hasBox) return power;
    const Ogre::Vector3 p = l->getParentNode()->_getDerivedPosition();
    Ogre::Vector3 closest = p;
    closest.makeCeil(c.boxMin);
    closest.makeFloor(c.boxMax);
    const float d = closest.distance(p);
    const float range = float(l->getAttenuationRange());
    // THE RANGE CULL (IMAGE-1's, every consumer's now): past its range a light
    // lights nothing (the window is 0), so missing the box drops nothing.
    if (!(d <= range)) { reaches = false; return 0.0f; }
    // THE CONE CULL (a spot), conservative: a spot whose apex is outside the box and
    // whose cone — widened by the box's angular radius seen from the apex — holds
    // none of the box's eight corners lights nothing in it.
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
            bool anyInside = false;
            for (int k = 0; k < 8 && !anyInside; ++k) {
                const Ogre::Vector3 corner((k & 1) ? c.boxMax.x : c.boxMin.x, (k & 2) ? c.boxMax.y : c.boxMin.y,
                                           (k & 4) ? c.boxMax.z : c.boxMin.z);
                Ogre::Vector3 v = corner - p;
                const float len = float(v.length());
                if (len <= 0.0f || float(v.dotProduct(dir)) >= cosLimit * len) anyInside = true;
            }
            if (!anyInside) { reaches = false; return 0.0f; }
        }
    }
    // THE RANK: the irradiance THE BOX receives, in one unit for every type — the
    // light's falloff at d_f = max(d, the box's half-extent / 2), so every lamp inside
    // or hugging the box is ranked by the light it delivers to the box as a whole and
    // not by its near field (a source radius is a near-field detail, not energy).
    const Ogre::Vector3 half = (c.boxMax - c.boxMin) * 0.5f;
    const float dF = std::max(d, 0.5f * float(std::max(half.x, std::max(half.y, half.z))));
    if (isArea(t)) {
        // JahBrdf_piece_all's jahAreaLightAttenuation, the pixel's own (AreaLights_piece_ps,
        // AREA-SCALE-1): a Lambertian emitter of radiance I seen on its axis delivers
        // I r^2 / (r^2 + d^2) times the window, r the disc of the rectangle's area — the
        // attenuation's "linear" slot (OgreScene packs sqrt(w h / pi) there), in the same
        // units a point light's I / d^2 is in (both carry the engine's I x pi power scale):
        // an area and a point light that deliver equal irradiance tie.
        const float r = float(l->getAttenuationLinear());
        const float r2 = r * r;
        return power * lightWindow(d, range) * r2 / std::max(r2 + dF * dF, 1e-8f);
    }
    // jahLightAttenuation's far field: the window over d^2.
    return power * lightWindow(d, range) / std::max(dF * dF, 1e-8f);
}

void SceneLightList::select(Ogre::SceneManager *sm, const Consumer &c, Selection &out) const {
    out.lights.clear();
    out.inRange = 0u;
    out.overBudget = 0u;
    refresh(sm);
    struct Cand {
        std::uint32_t entry = 0u;
        float score = 0.0f;
        float centreD2 = 0.0f;   ///< squared distance to the box's centre (the viewer of a cascade)
    };
    const Ogre::Vector3 centre = (c.boxMin + c.boxMax) * 0.5f;
    std::vector<Cand> cands;
    cands.reserve(mEntries.size());
    const Ogre::uint32 mask = c.lightMask & Ogre::VisibilityFlags::RESERVED_VISIBILITY_FLAGS;
    for (std::uint32_t i = 0; i < mEntries.size(); ++i) {
        const Ogre::Light *l = mEntries[i].light;
        // Visibility is read live as well (a hide bumps the dirt; this is the guard).
        if (!l->getVisible() || !(l->getVisibilityFlags() & mask)) continue;
        const Ogre::Light::LightTypes t = l->getType();
        const bool wanted = (t == Ogre::Light::LT_DIRECTIONAL && c.directional) ||
                            (isPointSpot(t) && c.pointSpot) || (isArea(t) && c.area);
        if (!wanted) continue;
        bool reaches = true;
        const float score = contribution(l, c, reaches);
        if (!reaches) continue;
        const float cd2 = l->getType() == Ogre::Light::LT_DIRECTIONAL
                              ? 0.0f
                              : float(l->getParentNode()->_getDerivedPosition().squaredDistance(centre));
        cands.push_back({ i, score, cd2 });
    }
    out.inRange = std::uint32_t(cands.size());
    if (c.budget && cands.size() > c.budget) {
        // THE BUDGET'S WORTH BY CONTRIBUTION; ties (every lamp inside the box reads the
        // floored peak) by the nearest to the box's centre — a cascade's is the viewer —
        // then the shared order.
        std::nth_element(cands.begin(), cands.begin() + c.budget, cands.end(),
                         [this](const Cand &a, const Cand &b) {
                             if (a.score != b.score) return a.score > b.score;
                             if (a.centreD2 != b.centreD2) return a.centreD2 < b.centreD2;
                             return mEntries[a.entry].rank < mEntries[b.entry].rank;
                         });
        cands.resize(c.budget);
        // ...EMITTED IN MEMORY ORDER.
        std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.entry < b.entry; });
    }
    out.overBudget = out.inRange - std::uint32_t(cands.size());
    out.lights.reserve(cands.size());
    for (const Cand &k : cands) out.lights.push_back(mEntries[k.entry].light);
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
