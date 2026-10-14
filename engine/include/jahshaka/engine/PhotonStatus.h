// PHOTON'S DISPATCH COUNT — the test door of ENGINE-UPGRADE-2B stage 3 (ONE GI REGION: every Photon
// compute dispatch is made inside PhotonComponent's GI region, the span the ASYNC bracket wraps).
// Counters only, process-wide (the dispatch sites are the voxel classes, which know no scene).
#ifndef JAHSHAKA_ENGINE_PHOTONSTATUS_H
#define JAHSHAKA_ENGINE_PHOTONSTATUS_H

namespace jahshaka {
namespace engine {

struct PhotonDispatchStatus {
    /// Photon compute dispatches made inside the GI region, ever.
    unsigned long long inRegion = 0;
    /// ...and outside every region (an event, a tool, a path the region does not cover): the bar is 0.
    unsigned long long outsideRegion = 0;
};
PhotonDispatchStatus photonDispatchStatus();

}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_PHOTONSTATUS_H
