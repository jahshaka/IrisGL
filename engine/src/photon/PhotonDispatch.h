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
class ComputeTools;
class TextureGpu;
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

/// THE ONE DISPATCH DOOR: a Photon compute dispatch with its barriers, noted by the witness.
void photonDispatch(Ogre::HlmsCompute *hc, Ogre::HlmsComputeJob *job, Ogre::RenderSystem *rs);
/// ...and its two siblings for ComputeTools' clears (a compute dispatch each, ENGINE-UPGRADE-ASYNC-3):
/// noted by the witness like every other, so the door's count is every Photon dispatch. The
/// caller has resolved the clears' barriers (ComputeTools::prepareForUavClear).
void photonClearUavFloat(Ogre::ComputeTools *tools, Ogre::TextureGpu *tex, const float value[4]);
void photonClearUavUint(Ogre::ComputeTools *tools, Ogre::TextureGpu *tex, const unsigned value[4]);

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_PHOTONDISPATCH_H
