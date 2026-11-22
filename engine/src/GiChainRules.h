#pragma once
// THE CHAIN'S DIFFUSE-GI ROUTE AS A PURE FUNCTION (ROUTE-ROWS-1 stage 0; no Ogre, no device): which producer lights a
// view's opaque passes. ChainDesc (OgreView::chainDesc) reads it; tests/engine test_chain_gi_routes.cpp holds it
// (engine.chain_gi_routes). The pattern is ReflectChainRules.h's.

namespace jahshaka::engine::detail {

/// The chain's two diffuse producers. At most one is true: one world, one producer per view.
struct ChainGiRoutes {
    bool probeGather = false;       ///< the screen-probe gather (rays)
    bool screenIntegrate = false;   ///< the no-ray route's screen integrate (the field and the cones)
};

/// ONE PRODUCER FOR EVERY VIEW OF A RAY SCENE (ROUTE-ROWS-1 stage 0; CODING_RULES §1, physics is the rule): wherever the
/// scene's GI is on and the scene traces (`gatherWanted` = OgreScene::probeGatherWanted) the gather IS the diffuse of
/// every view that draws it - a window, the headless host's presented stand-in, a stereo pair, and every offscreen
/// picture alike (a screenshot, a thumbnail, a preview widget, the startup warm-up, a test's fixed-frame render). The
/// view's kind is not an input: the Live/StillPicture contract's GI meaning, which pinned a live offscreen view off the
/// gather (a second producer, a different picture of the same world), is deleted - the contract itself stays, read by
/// the exposure meter alone (OgreEngine::meterSecondsFor). Where the scene does NOT trace and its GI is on (`giOn` =
/// OgreScene::screenIntegrateWanted), a MONO view takes the screen integrate (the same screen products from the field
/// and the cones; NO-RAY-CUT-1 removes it after the close); a stereo view keeps the pixel's own cones there (neither).
/// `integrateArmOff` is the measuring arm ArmId::GiScreenIntegrateOff.
constexpr ChainGiRoutes chainGiRoutes(bool gatherWanted, bool giOn, bool stereo, bool integrateArmOff)
{
    ChainGiRoutes r;
    r.probeGather = gatherWanted;
    r.screenIntegrate = !r.probeGather && !stereo && giOn && !integrateArmOff;
    return r;
}

}   // namespace jahshaka::engine::detail
