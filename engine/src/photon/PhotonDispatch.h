// THE GI REGION AND ITS ONE DISPATCH DOOR (ENGINE-UPGRADE-2B stage 3; photon/PhotonComponent.h).
// Photon's frame GPU work runs in ONE named region: the GI step at engine.pre (harvest -> plan ->
// record: the cascade update, the field, the injections, the mips, the border mirrors) and the arm
// build after it, each inside PhotonComponent::GiRegion; the cards' photon_update pass is a region
// of its own. Every Photon compute dispatch goes through photonDispatch (its barriers, the witness,
// the dispatch); one made outside every region is counted OUTSIDE (jahshaka/engine/PhotonStatus.h)
// and logged once. The ASYNC bracket wraps the region.
#ifndef JAHSHAKA_ENGINE_PHOTONDISPATCH_H
#define JAHSHAKA_ENGINE_PHOTONDISPATCH_H

namespace Ogre {
class HlmsCompute;
class HlmsComputeJob;
class RenderSystem;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
namespace detail {

void notePhotonDispatch();
unsigned long long photonDispatchesInRegion();
unsigned long long photonDispatchesOutsideRegion();
/// The region's open depth (nesting counts once): PhotonComponent::beginGiRegion / endGiRegion.
void photonRegionBegin();
void photonRegionEnd();

/// THE ONE DISPATCH DOOR: a Photon compute dispatch with its barriers, noted by the witness.
void photonDispatch(Ogre::HlmsCompute *hc, Ogre::HlmsComputeJob *job, Ogre::RenderSystem *rs);

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_PHOTONDISPATCH_H
