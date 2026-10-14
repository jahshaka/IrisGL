// PHOTON, THE COMPONENT (photon/PhotonComponent.h has the design).
#include "photon/PhotonComponent.h"

#include "EnginePrivate.h"
#include "SurfaceCache.h"

namespace jahshaka {
namespace engine {
namespace detail {

const char *const PhotonComponent::kUpdatePass = "photon_update";
const char *const PhotonComponent::kTracePass = "photon_trace";
const char *const PhotonComponent::kResolvePass = "photon_resolve";

void PhotonComponent::registerPasses(AtomPassProvider *provider) {
    registerPhotonRayPasses(provider);
    registerPhotonUpdatePass(provider);
}

void PhotonComponent::volumes(std::vector<Volume> &out) const {
    out.clear();
    mIn.builtVolumes(out);
}

/// THE ADAPTER (until U0 merges): the component's inputs are today's scene state.
PhotonComponent::Inputs OgreScene::photonInputs() {
    PhotonComponent::Inputs in;
    in.builtVolumes = [this](std::vector<PhotonComponent::Volume> &out) {
        for (const VctCascade &c : mVctCascades)
            if (c.built) out.push_back(PhotonComponent::Volume{ c.lighting, c.voxelizer });
    };
    in.surfaceCache = [this]() { return mSurfaceCache.get(); };
    in.materials = [this]() { return mVctMaterialStore; };
    in.driverStereo = [this]() { return mGiDriverStereo; };
    return in;
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
