#pragma once
// THE RAY CONSUMER's DOOR (SHADER-SYSTEM-1 stage 7, call 3; the lead's ruling P1): what a ray consumer does at its call
// (RayQueryTier::aheadReady) with the pipeline build it needs. A driver compile of a ray pipeline is wall-clock work of up
// to ~110 s on a cold driver (rq_reflect: ~/.local/share/jahlab's owner log, 2026-11-22), and a join of it on the render
// thread is a frozen window - a frozen HEADSET (VR first). So in a scene whose compute goes through the background
// compiler (its views compile in the background and the service runs: OgreScene::computeOnService) a build still running
// is NEVER joined, AT ANY PACE: the consumer takes its own "not this frame" path and names its scene (the frame is
// incomplete, PublishedFrame::Completeness bit 2) until the build lands; a frame that must be complete is TAKEN at the
// first complete published frame, frames flowing meanwhile. A synchronous scene (a script, MCP, the selftest, a fixture,
// the lab's --headless) KEEPS the join ON PURPOSE: its frames are counted and hashed, so when a build lands is a function
// of the frames, never of a compile's wall time. A finished build is joined at once (no wait: its lines are logged).
// The law is a CPU row: engine.ray_ahead_door (tests/engine/test_ray_ahead_door.cpp).

namespace jahshaka { namespace engine { namespace rayahead {

enum class Door {
    Run,           ///< no build in flight (joined already, or never started): the consumer reads its setup
    NotThisFrame,  ///< the build still runs in a scene on the service: the consumer's own path, its scene named
    Join           ///< join the build now (finished: no wait; running in a synchronous scene: the wait, on purpose)
};

/// `pending`: the build was started and is not joined; `running`: it has not finished (or the test door rays.buildHold
/// counts it in flight); `onService`: the scene's compute is on the background compiler; `frameOwesPicture`: the frame's
/// pace is Complete - NOT READ, and that is the law (before stage 7 a Complete frame joined: the editor's frames, and the
/// startup gate's Complete tails, waited for a cold driver compile).
inline Door door(bool pending, bool running, bool onService, bool frameOwesPicture)
{
    (void)frameOwesPicture;
    if (!pending) return Door::Run;
    if (running && onService) return Door::NotThisFrame;
    return Door::Join;
}

}}}  // namespace jahshaka::engine::rayahead
