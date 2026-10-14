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
    for (const VctCascade &c : mVctCascades)
        if (c.built) out.push_back(Volume{ c.lighting, c.voxelizer });
}

PhotonComponent::PhotonComponent() = default;
// Out of line: the cache's and the feeds' types are complete here.
PhotonComponent::~PhotonComponent() = default;

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
