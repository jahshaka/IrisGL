// PHOTON'S DISPATCH COUNT — the test door of ENGINE-UPGRADE-2B stage 3 (one recording entry point:
// every Photon GPU dispatch records inside the photon_update pass). Counters only, process-wide
// (the dispatch sites are the voxel classes, which know no scene). No Ogre types.
#ifndef JAHSHAKA_ENGINE_PHOTONSTATUS_H
#define JAHSHAKA_ENGINE_PHOTONSTATUS_H

namespace jahshaka {
namespace engine {

struct PhotonDispatchStatus {
    /// Photon GPU dispatches recorded inside a photon_update pass, ever.
    unsigned long long inPass = 0;
    /// ...and with no photon_update pass open (engine.pre, an arm build, an event): the bar is 0.
    unsigned long long outsidePass = 0;
};
PhotonDispatchStatus photonDispatchStatus();

}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_PHOTONSTATUS_H
