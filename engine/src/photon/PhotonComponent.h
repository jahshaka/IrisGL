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
//   photon_update  — first in the GI workspace (the surface cache's, position 0): the
//                    cards' sun trace and relight.
//   photon_trace   — per view, in front of the pass that shades the hits (the hit decode
//                    pass, or the first pass that reads the ray jobs' answer).
//   photon_resolve — per view, in front of the first pass that reads the answer: the
//                    write-back, the filters, the pass-scoped bindings.
// The compositor's own listing is the order (gi.photon_passes asserts it).
//
// THE ADAPTER (until U0 merges — SPECS/ENGINE-UPGRADE-2.md THE BOUNDARY). The component
// will take only (the frame's record, the change set, a read-only GPU-scene view); until
// the record exists its inputs are today's scene state, read through `Inputs` — the same
// interface the record will feed. The orchestration that decides WHAT is rebuilt (the
// cascade scheduler, the re-voxelisation, the field's update) stays OgreGi.cpp's until
// stage 4 moves it into `plan()`; this stage does not invent an empty one.
#ifndef JAHSHAKA_ENGINE_PHOTONCOMPONENT_H
#define JAHSHAKA_ENGINE_PHOTONCOMPONENT_H

#include <functional>
#include <vector>

namespace Ogre {
class PhotonVoxelLighting;
class PhotonVoxelMaterial;
class PhotonVoxelizer;
class SceneManager;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
class SurfaceCache;
namespace detail {


class PhotonComponent {
public:
    /// One built voxel cascade, innermost first: what a ray reads the cascade chain by.
    struct Volume {
        Ogre::PhotonVoxelLighting *lighting = nullptr;
        Ogre::PhotonVoxelizer *voxelizer = nullptr;
    };
    /// THE ADAPTER'S INPUTS: today's scene state, behind the interface the frame's
    /// record replaces at stage 4. Every one is a read; none may be empty.
    struct Inputs {
        std::function<void(std::vector<Volume> &)> builtVolumes;
        std::function<SurfaceCache *()> surfaceCache;
        std::function<Ogre::PhotonVoxelMaterial *()> materials;
        /// A headset drives the scene's GI (the VR column of the quality table).
        std::function<bool()> driverStereo;
    };
    explicit PhotonComponent(Inputs in) : mIn(std::move(in)) {}
    PhotonComponent(const PhotonComponent &) = delete;
    PhotonComponent &operator=(const PhotonComponent &) = delete;

    // --- THE PRODUCTS ---------------------------------------------------------
    /// The built cascades, innermost first (cleared first).
    void volumes(std::vector<Volume> &out) const;
    /// The surface cache (the cards), or null where the row is off.
    SurfaceCache *surfaceCache() const { return mIn.surfaceCache(); }
    /// The chain's one voxel material store, or null before the first voxeliser.
    Ogre::PhotonVoxelMaterial *materials() const { return mIn.materials(); }
    bool driverStereo() const { return mIn.driverStereo(); }

    // --- THE PASSES -------------------------------------------------------------
    static const char *const kUpdatePass;    ///< "photon_update"
    static const char *const kTracePass;     ///< "photon_trace"
    static const char *const kResolvePass;   ///< "photon_resolve"

private:
    Inputs mIn;
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_PHOTONCOMPONENT_H
