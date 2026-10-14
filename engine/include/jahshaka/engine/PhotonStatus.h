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

class Scene;
/// THE IRRADIANCE FIELD'S VARIANCE STOP (ENGINE-UPGRADE-2B stage 4): the generation job's own
/// counts, read back from the GPU (lagging a few frames). Zero when the scene has no field.
struct PhotonFieldStatus {
    bool field = false;
    unsigned probes = 0;
    /// Probes integrations made, ever (the job's own count: a stopped or converged probe a
    /// refinement skips is not one).
    unsigned long long integrations = 0;
    /// Probes stopped by the variance stop, by the last reading that no fresh work outdated.
    unsigned probesStopped = 0;
    /// The stop's rule: the fewest samples (0: the stop is off) and the precision (2 SE / mean).
    unsigned stopMinSamples = 0;
    float stopPrecision = 0.0f;
};
PhotonFieldStatus photonFieldStatus(const Scene &scene);

}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_PHOTONSTATUS_H
