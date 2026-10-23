// PHOTON'S DISPATCH COUNT — the test door of ENGINE-UPGRADE-2B stage 3 (ONE GI REGION: every Photon
// compute dispatch is made inside PhotonComponent's GI region; its Compute span is compute-legal).
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
    /// The mip chains' dispatches (ENGINE-UPGRADE-ASYNC-3 stage 1: one a level, through the door), ever.
    unsigned long long mipDispatches = 0;
    /// The mip jobs ever pooled (one per format, volume size and level: a rebuild adds none).
    unsigned long long mipJobs = 0;
    /// Brackets the region check opened (setPhotonRegionBracketCheck), ever.
    unsigned long long regionBrackets = 0;
    /// THE COMPUTE REGION'S TWO COUNTS (ENGINE-UPGRADE-ASYNC-3 stage 1): the door's dispatches made
    /// while a Compute region was open, and the RENDER SYSTEM's own dispatches recorded in the same
    /// spans (the fork's VulkanRenderSystem count) - equal when nothing dispatches around the door.
    unsigned long long doorInCompute = 0;
    unsigned long long renderSystemInCompute = 0;
};
PhotonDispatchStatus photonDispatchStatus();

/// THE REGION'S BRACKET CHECK (ENGINE-UPGRADE-ASYNC-3 stage 1) — stage 1's acceptance instrument for
/// gi.region_compute_only, the row's door and not a shipped path (nothing in the product sets it): while on, the
/// outermost open of every GI region opens the fork's async compute bracket on its IN-ORDER arm (the
/// graphics queue still records; the queue's async arm is switched off while the check runs) and its
/// close ends it. The fork refuses every graphics-only command inside a bracket in either arm (an
/// Ogre exception naming it, logged), so a region that records one is seen. Off restores the arm.
void setPhotonRegionBracketCheck(bool on);

/// THE GI BINDING BY READER (ENGINE-UPGRADE-ASYNC-3 stage 2): a scene's GI (its voxel volumes, its
/// field) is bound only on the passes that sample it. Counts of the PBS/Atom pass preparations, ever,
/// for a pass whose scene HAS a GI binding: on a reader pass (bound) and on a pass marked as sampling
/// none (the G-buffer prepasses, the card capture, the overlays: not bound).
struct PhotonBindingStatus {
    unsigned long long readerPrepares = 0;
    unsigned long long nonReaderPrepares = 0;
};
PhotonBindingStatus photonBindingStatus();

class Scene;
/// THE IRRADIANCE FIELD'S VARIANCE STOP (ENGINE-UPGRADE-2B stage 4): the generation job's own
/// counts, read back from the GPU (lagging a few frames). Zero when the scene has no field.
struct PhotonFieldStatus {
    bool field = false;
    unsigned probes = 0;
    /// Probes integrations made, ever (the job's own count: a stopped or converged probe a
    /// refinement skips is not one).
    unsigned long long integrations = 0;
    /// Probes stopped by the variance stop, by the last reading, less those restarted after it (the per-slot ledger).
    unsigned probesStopped = 0;
    /// The stop's rule: the fewest samples (0: the stop is off) and the precision (the 95 % Student-t interval / mean).
    unsigned stopMinSamples = 0;
    float stopPrecision = 0.0f;
};
PhotonFieldStatus photonFieldStatus(const Scene &scene);

}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_PHOTONSTATUS_H
