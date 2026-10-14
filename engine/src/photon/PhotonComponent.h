// PHOTON, THE COMPONENT (ENGINE-UPGRADE-2 stage 2, SPECS/ENGINE-UPGRADE-2.md; the Terra
// pattern: a facility of ours with a type and a boundary, never OgreScene's members read
// from outside).
//
// WHAT IT IS. The scene's Photon GI — the voxel cascades, the irradiance field, the
// surface cache, the gather's inputs — as ONE object with ONE interface. Every reader
// outside Photon (the ray tier's traces and resolves, a view's binding) takes Photon's
// PRODUCTS through it; nobody reaches into the scene's members (the ray tier's friend
// access is gone: GPU-scene facts come through the substrate's view on OgreScene, the
// GI products through here).
//
// ITS GPU WORK IS THREE COMPOSITOR PASSES (PASS_CUSTOM, served by the engine's one pass
// provider, AtomPass.h):
//   photon_update  — the pass of the GI workspace right after the card capture's (the
//                    surface cache's two workspaces, positions 0 and 1): the cards' sun
//                    trace, the batch's close, the relight. The voxel cascades' and the
//                    irradiance field's dispatches are NOT in it yet: OgreGi.cpp's
//                    orchestration records them BEFORE the frame (engine.pre), each where
//                    its parameters are written — the voxeliser's compute jobs are shared
//                    across cascades and re-parameterised per dispatch, the field maps its
//                    parameter buffer per update — so a dispatch cannot be deferred alone;
//                    they move into the pass with the orchestration, at stage 4.
//   photon_trace   — per view, in front of the pass that shades the hits (the hit decode
//                    pass, or the first pass that reads the ray jobs' answer).
//   photon_resolve — per view, in front of the first pass that reads the answer: the
//                    write-back, the filters, the pass-scoped bindings.
// The compositor's own listing is the order (gi.photon_passes asserts it).
//
// ITS STATE IS ITS OWN (ENGINE-UPGRADE-2B stage 1): the voxel cascades, the surface cache,
// the voxel material store and the driving view's profile are the component's members, read
// by the products below (the ray tier's traces read nothing else). The scene that holds the
// component is its one friend while the orchestration that decides WHAT is rebuilt (the
// cascade scheduler, the re-voxelisation, the field's update) is still the scene's to call
// (OgreGi.cpp); stage 2 moves it into `plan()`.
#ifndef JAHSHAKA_ENGINE_PHOTONCOMPONENT_H
#define JAHSHAKA_ENGINE_PHOTONCOMPONENT_H

#include "jahshaka/engine/Types.h"

#include <OgrePrerequisites.h>
#include <OgreVector3.h>

#include <memory>
#include <vector>

namespace Ogre {
class CompositorWorkspace;
class PhotonVoxelLighting;
class PhotonVoxelMaterial;
class PhotonVoxelizer;
class SceneManager;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
class SurfaceCache;
namespace detail {

class OgreScene;
class VoxelFeed;

class AtomPassProvider;
/// The ray passes' recorders (OgreRayQuery.cpp: photon_trace, photon_resolve).
void registerPhotonRayPasses(AtomPassProvider *provider);
/// The update pass's recorder (OgreSurfaceCache.cpp: photon_update).
void registerPhotonUpdatePass(AtomPassProvider *provider);
/// photon_update's GI workspace (null without a surface cache): the monitor times it.
Ogre::CompositorWorkspace *photonUpdateWorkspace(const SurfaceCache *cache);


class PhotonComponent {
public:
    /// One built voxel cascade, innermost first: what a ray reads the cascade chain by.
    struct Volume {
        Ogre::PhotonVoxelLighting *lighting = nullptr;
        Ogre::PhotonVoxelizer *voxelizer = nullptr;
    };
    PhotonComponent();
    ~PhotonComponent();
    PhotonComponent(const PhotonComponent &) = delete;
    PhotonComponent &operator=(const PhotonComponent &) = delete;

    // --- THE PRODUCTS ---------------------------------------------------------
    /// The built cascades, innermost first (cleared first).
    void volumes(std::vector<Volume> &out) const;
    /// The surface cache (the cards), or null where the row is off.
    SurfaceCache *surfaceCache() const { return mSurfaceCache.get(); }
    /// The chain's one voxel material store, or null before the first voxeliser.
    Ogre::PhotonVoxelMaterial *materials() const { return mVctMaterialStore; }
    /// A headset drives the scene's GI (the VR column of the quality table).
    bool driverStereo() const { return mGiDriverStereo; }

    // --- THE PASSES -------------------------------------------------------------
    static const char *const kUpdatePass;    ///< "photon_update"
    static const char *const kTracePass;     ///< "photon_trace"
    static const char *const kResolvePass;   ///< "photon_resolve"
    /// Registers the three passes' recorders and gates on the engine's pass provider
    /// (AtomPassProvider::install calls it: the provider is Ogre's one per manager).
    static void registerPasses(AtomPassProvider *provider);

    // --- ITS TYPES --------------------------------------------------------------
    struct VctCascade {
        Ogre::PhotonVoxelizer *voxelizer = nullptr;   ///< owned, except [0]
        Ogre::PhotonVoxelLighting  *lighting  = nullptr;   ///< owned, except [0]
        float        halfSize   = 0.0f;
        Ogre::uint32 resolution = 64u;
        float        stepCells  = 4.0f;
        /// The lattice cell this cascade is centred on (integer coordinates in
        /// units of `step`), and the world centre that resolves to.
        long long    latticeX = 0, latticeY = 0, latticeZ = 0;
        Ogre::Vector3 centre = Ogre::Vector3::ZERO;
        bool         built   = false;
        /// THIS CASCADE'S FEED (ATOM P4b): the records a gather writes on the
        /// device and this cascade's voxeliser reads, the ranges, and the readout
        /// of what it voxelised. ONE PER CASCADE so a rebuild can never overwrite
        /// records another cascade's recorded dispatches are about to read. Owned
        /// by pointer so the vector of cascades can move without copying buffers.
        /// `itemsAttached`, `itemsStale` and `attachedItems` are GONE: nothing is
        /// attached any more (the gather re-reads every predicate from the GPU
        /// scene on every rebuild), so there is no set to hold, stale or compare.
        std::unique_ptr<detail::VoxelFeed> feed;
        /// WHAT THIS CASCADE VOXELISED - items, attach set, levels, triangles - is
        /// no longer a CPU member: it is the feed's READOUT, counted on the device
        /// as the records were written and read by giStatus (`VoxelFeed::reading`).
        /// This cascade is BEHIND the camera and owes a rebuild — a flag, not a
        /// queue: a rebuild always happens at the CURRENT camera, so owing two
        /// of them is the same as owing one.
        int          pending = 0;
        /// WHY it is pending — the reason the monitor row and the event carry.
        /// `Camera` for an ordinary scroll (E0's B12/D5), the edit's own reason
        /// when the dirty path marked it (G1), so a capture can separate the
        /// cost of walking around a scene from the cost of changing it.
        GiStaleReason pendingReason = GiStaleReason::Camera;
        /// This cascade's queued rebuild came from the JUMP guard, not from an
        /// ordinary scroll — i.e. nothing of its old volume was reusable.
        /// Cleared when the rebuild is serviced, and counted there, so the
        /// counter is one per cascade per teleport rather than one per frame
        /// the camera spends far away.
        bool         jumped  = false;
        /// Consecutive failed rebuilds. One retry on the next frame, then the
        /// cascade stands down and waits for the camera to move again (round-2
        /// review F3) — a cascade that cannot build must not spend the frame's
        /// whole GI budget for ever and starve the ones that still can.
        unsigned     failures = 0;
        unsigned long long rebuilds = 0;
        /// THIS CASCADE HAS BEEN INJECTED SINCE THE LAST IN-MOTION LIGHT TICK
        /// (DRAG-1, REFLECT F2). `rebuildCascade` ends with a full-count
        /// `PhotonVoxelLighting::update`, so a cascade the scheduler rebuilt this frame
        /// already holds the answer the tick would compute — re-computing it is
        /// duplicate work, and computing a DIFFERENT one (which is what the
        /// zero-bounce moving tick did) is the pulse. Set by every rebuild,
        /// cleared by the tick that skipped on it.
        bool         injectedSinceTick = false;
        /// ...AND THE LIGHT STATE IT WAS INJECTED AT (DRAG-1 round 2, F5). The
        /// latch above says "a rebuild already computed what the tick would
        /// compute", and that is only true while nothing the INJECTION reads has
        /// changed since. A lamp that moved or was re-parameterised between the
        /// rebuild and the tick makes the rebuild's answer stale, and skipping
        /// on the latch alone would leave that cascade holding the old light
        /// until its next rebuild — which, for an outer cascade deferred behind
        /// cascade 0, is the whole drag. The tick skips only when this still
        /// equals the scene's current light-write serial.
        unsigned long long injectedAtLightSerial = 0;
        /// THE ONE-WRITER LATCH (PHOTON-WRITER-1): the writer frame
        /// (`giWriterFrame()`) this cascade was last injected in. `injectCascade`
        /// refuses a second injection of one cascade inside one frame. The two
        /// fields above STAY beside it because they say something a per-frame
        /// latch cannot: "a rebuild has injected this cascade since the last
        /// MOVING TICK", and the ticks run every few frames, so that fact spans
        /// frames.
        unsigned long long injectedFrame = ~0ull;
        float        lastCpuMs = -1.0f;
        /// The camera position this cascade was last BUILT for. The scroll test
        /// quantises BOTH on an absolute world lattice of `step * (1 -
        /// kStepHysteresis)` metres and requires the camera to be the band past
        /// the plane it left (OgreGi.cpp) — the pin's `consistentCascadeSteps`
        /// reading with the hysteresis added, which is what keeps two cascades
        /// from stepping on different frames for the same metre AND what stops a
        /// head swaying on a plane from thrashing the chain. Because the lattice
        /// is absolute, `step()` is the SUPREMUM of the travel between rebuilds,
        /// not the distance between them.
        Ogre::Vector3 builtCam = Ogre::Vector3::ZERO;
        float step() const { return stepCells * (halfSize * 2.0f / float(resolution)); }
        float cell() const { return halfSize * 2.0f / float(resolution); }
    };

private:
    friend class OgreScene;
    /// THE PHOTON CASCADE CHAIN, innermost first. Empty when no arm is built.
    /// [0] mirrors the scene's mVctVoxelizer/mVctLighting (NOT owned through here).
    std::vector<VctCascade> mVctCascades;
    std::unique_ptr<SurfaceCache> mSurfaceCache;
    Ogre::PhotonVoxelMaterial *mVctMaterialStore = nullptr;
    /// Is the view that DRIVES GI a stereo (headset) one? It picks the tier
    /// table's VR column (GiViewProfile, V1-RIG item 4) and is written by the
    /// once-a-frame driver hook.
    bool mGiDriverStereo = false;
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_PHOTONCOMPONENT_H
