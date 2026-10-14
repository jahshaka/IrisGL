// THE ONE RECORDING ENTRY POINT'S WITNESS (ENGINE-UPGRADE-2B stage 3; photon/PhotonComponent.h).
// Every Photon GPU dispatch — the voxeliser's AABB and voxelise jobs, the gather's, the light
// injection, the bounce, the anisotropic mips, the irradiance field's generation and its border
// mirrors, the voxel material store's conversions — notes itself here; one recorded while no
// photon_update pass is open is counted OUTSIDE (jahshaka/engine/PhotonStatus.h). The ASYNC
// bracket wraps photon_update, so outside must be 0.
#ifndef JAHSHAKA_ENGINE_PHOTONDISPATCH_H
#define JAHSHAKA_ENGINE_PHOTONDISPATCH_H

namespace jahshaka {
namespace engine {
namespace detail {

void notePhotonDispatch();
unsigned long long photonDispatchesInPass();
unsigned long long photonDispatchesOutsidePass();
/// photon_update's recorder holds one for the length of its work (nesting counts once).
struct PhotonPassScope {
    PhotonPassScope();
    ~PhotonPassScope();
    PhotonPassScope(const PhotonPassScope &) = delete;
    PhotonPassScope &operator=(const PhotonPassScope &) = delete;
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_PHOTONDISPATCH_H
