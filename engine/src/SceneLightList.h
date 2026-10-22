// THE SCENE'S ONE LIGHT LIST (P1C-LIGHT-LIST, V2 P1c; SPECS/v2/P1_VOXEL_CHAIN_DESIGN.md; PHOTON-7 stage 1).
//
// WHAT THIS IS. Every consumer that lights the WORLD rather than the camera's
// frustum — the voxel cascades' light injection (PhotonVoxelLighting::update),
// the surface cache's card relight (SurfaceCache::relightCards), the ray tier's hit
// table and Atom's hit decode (HlmsAtom::buildWorldLights) — reads the scene's
// lights from here, in ONE ORDER and by ONE RULE:
//
//   * THE ORDER (PHOTON-7 stage 1, L1: a world function): every light of the scene's
//     SceneManager that is visible (LAYER_VISIBILITY) and attached, ranked by PEAK
//     INTENSITY x RANGE^2 — the peak channel of diffuse x power scale times the square of
//     its range, the light's reach — brightest first, a directional light (no range: it
//     reaches everything) before every ranged one, the id as the tie. It depends on the
//     lights alone, never on a box or a camera.
//   * PER CONSUMER (select): the lights whose range reaches the consumer's box — a
//     directional light always does; a point, spot or area light whose range sphere misses
//     the box lights nothing in it (JahBrdf's window is 0 past the range), so it is CULLED,
//     which drops no light (a spot also when its cone, widened by the box's angular radius,
//     holds none of the box's corners) — in THE ORDER, and when more reach the box than the
//     consumer's BUDGET holds, the budget's FIRST of the order; the rest are COUNTED
//     (Selection::overBudget) and every consumer reports the count — a named overflow,
//     never a silent drop. (The per-box rank by the irradiance a light delivers to the box
//     is DELETED, PHOTON-7 stage 1: which lights a box held depended on where the box stood.)
//     The voxel injection then lights each voxel with the FIRST K of its lights that reach
//     the VOXEL (LightInjection's per-voxel count): a function of the world point.
//
// FRESHNESS. The list holds Light pointers, so it is rebuilt whenever the scene's
// light dirt moves (SceneGiBinding: the light write serial — every setLight push, a
// light node's pose or visibility write — and the count of lights destroyed); only
// OgreScene creates and destroys lights, and both moves bump that dirt. A list with
// no dirt to watch (a bare SceneManager) rebuilds on every read. `select` reads every
// pose and range LIVE from the Light, so the snapshot decides only membership and the
// order — both of which only a dirt-bumping write changes.
#pragma once

#include <cstdint>
#include <vector>

#include "OgreLight.h"
#include "OgreVector3.h"

namespace Ogre { class SceneManager; }

namespace jahshaka {
namespace engine {

class SceneLightList {
public:
    struct Entry {
        Ogre::Light *light = nullptr;
        /// Its index in THE ORDER (peak intensity x range^2 desc, directional first, id asc): 0 = the first.
        std::uint32_t rank = 0u;
    };
    /// What one consumer reads the list as.
    struct Consumer {
        /// The box the consumer lights (world space); `boxMin > boxMax` on any axis
        /// = no box: nothing is culled by range.
        Ogre::Vector3 boxMin = Ogre::Vector3(1.0f, 1.0f, 1.0f);
        Ogre::Vector3 boxMax = Ogre::Vector3(-1.0f, -1.0f, -1.0f);
        /// The most lights the consumer holds - the first of THE ORDER (0 = unbounded).
        std::uint32_t budget = 0u;
        /// The light's visibility flags must share a bit with this mask.
        std::uint32_t lightMask = 0xFFFFFFFFu;
        bool directional = true;
        bool pointSpot = true;
        bool area = true;
    };
    struct Selection {
        /// The chosen lights, in THE ORDER.
        std::vector<Ogre::Light *> lights;
        /// The lights of the consumer's types and mask whose range reaches its box.
        std::uint32_t inRange = 0u;
        /// Of those, the ones the budget could not hold (inRange - lights.size()).
        std::uint32_t overBudget = 0u;
    };

    /// The scene's dirt (OgreScene's counters; null = no dirt: rebuild per read).
    void bindDirt(const unsigned long long *serial, const unsigned long long *removed) {
        mSerial = serial;
        mRemoved = removed;
        mValid = false;
    }
    /// Drop the snapshot (the scene's teardown: no pointer outlives its lights).
    void reset() { mEntries.clear(); mByRank.clear(); mValid = false; mSm = nullptr; }

    /// The list, rebuilt first if its dirt moved. Memory order (the light memory manager's walk).
    const std::vector<Entry> &entries(Ogre::SceneManager *sm) const;
    /// The same lights in THE ORDER.
    const std::vector<Ogre::Light *> &byRank(Ogre::SceneManager *sm) const;
    /// One consumer's lights: range-culled against its box, the budget's first of THE ORDER, in THE ORDER.
    void select(Ogre::SceneManager *sm, const Consumer &c, Selection &out) const;

    /// THE ORDER's key: peak intensity x range^2 (+inf for a directional light). Exposed for the tests' reasoning.
    static float orderKey(const Ogre::Light *l);
    /// Whether `l` reaches the consumer's box (the range and cone culls `select` applies).
    static bool reaches(const Ogre::Light *l, const Consumer &c);
    /// How many times the list was rebuilt (a test's handle on the dirt rule).
    unsigned long long rebuilds() const { return mRebuilds; }

private:
    void refresh(Ogre::SceneManager *sm) const;

    const unsigned long long *mSerial = nullptr;
    const unsigned long long *mRemoved = nullptr;
    mutable Ogre::SceneManager *mSm = nullptr;
    mutable unsigned long long mBuiltSerial = 0ull, mBuiltRemoved = 0ull;
    mutable bool mValid = false;
    mutable std::vector<Entry> mEntries;
    mutable std::vector<Ogre::Light *> mByRank;
    mutable unsigned long long mRebuilds = 0ull;
};

/// The scene's list for `sm` — OgreScene's, through the scene's GI binding record;
/// a process-wide undirtied list for a SceneManager that registered none (rebuilt
/// on every read, so a harness gets the same rule and never a stale pointer).
const SceneLightList &sceneLightListOf(Ogre::SceneManager *sm);

}   // namespace engine
}   // namespace jahshaka
