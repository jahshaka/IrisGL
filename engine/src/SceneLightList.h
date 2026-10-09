// THE SCENE'S ONE LIGHT LIST (P1C-LIGHT-LIST, V2 P1c; SPECS/v2/P1_VOXEL_CHAIN_DESIGN.md).
//
// WHAT THIS IS. Every consumer that lights the WORLD rather than the camera's
// frustum — the voxel cascades' light injection (PhotonVoxelLighting::update),
// the surface cache's card relight (SurfaceCache::relightCards) and Atom's hit
// decode (HlmsAtom::buildWorldLights) — reads the scene's lights from here. Before
// it there were three lists with three rules: the voxels took the first 16 lights
// in memory order, the cards the first 64 with no range cull, the hit decode every
// point and spot light sorted by power. Now there is one list and one rule:
//
//   * THE LIST: every light of the scene's SceneManager that is visible
//     (LAYER_VISIBILITY) and attached, in the light memory manager's order (the
//     order Ogre's own collections walk), each with its rank in THE SHARED ORDER —
//     peak channel of diffuse x power scale, brightest first, the id as the tie
//     (what the hit decode always sorted by).
//   * PER CONSUMER (select): the lights whose range reaches the consumer's box —
//     a directional light always does; a point, spot or area light whose range
//     sphere misses the box lights nothing in it (JahBrdf's window is 0 past the
//     range), so it is CULLED, which drops no light (a spot also when its cone,
//     widened by the box's angular radius, holds none of the box's corners) — then,
//     when more of them reach the box than the consumer's BUDGET holds, the budget's
//     worth that deliver the most irradiance TO THE BOX: the ONE falloff the shaders
//     take (JahBrdf_piece_all: the window over d^2 for a point or spot, the disc form
//     factor I r^2 / (r^2 + d^2) for an area light, in one unit, so a point and an area
//     light delivering equal irradiance tie) at d_f = max(d to the box, half the box's
//     half-extent) — a lamp inside or hugging the box is ranked by what it gives the
//     box as a whole, never by its near field; a tie goes to the lamp nearest the
//     box's centre (a cascade's is the viewer), then the shared order. The lights over
//     the budget are COUNTED (Selection::overBudget) and every consumer reports the
//     count — a named overflow, never a silent drop.
//   * THE CHOSEN SET IS EMITTED IN THE LIST'S MEMORY ORDER, so a consumer whose
//     in-range lights all fit packs exactly the bytes, in exactly the order, the
//     walk it replaced packed (no picture change where the budget is not reached).
//
// FRESHNESS. The list holds Light pointers, so it is rebuilt whenever the scene's
// light dirt moves (SceneGiBinding: the light write serial — every setLight push, a
// light node's pose or visibility write — and the count of lights destroyed); only
// OgreScene creates and destroys lights, and both moves bump that dirt. A list with
// no dirt to watch (a bare SceneManager) rebuilds on every read. `select` reads every
// pose, range and power LIVE from the Light, so the snapshot decides only
// membership and the shared order — both of which only a dirt-bumping write changes.
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
        /// Its index in the shared order (power desc, id asc): 0 = the brightest.
        std::uint32_t rank = 0u;
    };
    /// What one consumer reads the list as.
    struct Consumer {
        /// The box the consumer lights (world space); `boxMin > boxMax` on any axis
        /// = no box: nothing is culled by range and the contribution is the power.
        Ogre::Vector3 boxMin = Ogre::Vector3(1.0f, 1.0f, 1.0f);
        Ogre::Vector3 boxMax = Ogre::Vector3(-1.0f, -1.0f, -1.0f);
        /// The most lights the consumer holds (0 = unbounded).
        std::uint32_t budget = 0u;
        /// The light's visibility flags must share a bit with this mask.
        std::uint32_t lightMask = 0xFFFFFFFFu;
        bool directional = true;
        bool pointSpot = true;
        bool area = true;
    };
    struct Selection {
        /// The chosen lights, in the list's memory order.
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

    /// The list, rebuilt first if its dirt moved. Memory order.
    const std::vector<Entry> &entries(Ogre::SceneManager *sm) const;
    /// The same lights in the shared order (power desc, id asc).
    const std::vector<Ogre::Light *> &byRank(Ogre::SceneManager *sm) const;
    /// One consumer's lights: range-culled against its box, the budget's worth by
    /// contribution, emitted in memory order.
    void select(Ogre::SceneManager *sm, const Consumer &c, Selection &out) const;

    /// The contribution `select` ranks by (exposed for the tests' reasoning).
    static float contribution(const Ogre::Light *l, const Consumer &c, bool &reaches);
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
