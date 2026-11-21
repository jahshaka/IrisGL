#pragma once
// THE ROUGH LOBE's HISTORY STEP, as a pure function (ROUGH-LOBE-HISTORY-1, V2 plan 1.8): the one-line update of
// rq_probe_integrate.comp's rough specular history, per channel. The shader is GLSL and cannot include this header; its
// lines are pinned to these by source.rough_lobe_transport, and the estimator's law is proved on these by
// engine.rough_lobe_transport (Studio tests/engine). No Ogre, no device.
//
// The rough lobe's running mean was blended in the view it was made in - mix( prevLobe, lobe, blend ) - so on a walk it
// lagged the current view by ( N - 1 ) dL at the floor blend 1 / N (dL the lobe's change a frame): the soft reflection
// trailed. The history is CARRIED to the current view before the blend: the lobe's change since the history's view, both
// views evaluated on this frame's taps (roughLobeNow's loop of two), is added where the history is read. Without a carry
// (no history, or this eye's view did not move: the camera's bits equal) the change is the literal +0.0, so the carried
// history IS the history, bit for bit.

namespace jahshaka::engine::detail {

/// roughLobeNow's out delta: lobe( V ) - lobe( Vprev ) with a carry, the literal +0.0 without one.
inline float roughLobeDelta(float lobeNow, float lobeAtPrev, bool carry) { return carry ? lobeNow - lobeAtPrev : 0.0f; }

/// The history where it is read (prevLobe): the stored mean plus the lobe's change - left unclamped before the mix.
inline float roughLobeCarried(float stored, float delta) { return stored + delta; }

/// The blend (GLSL mix, the driver's x * (1 - a) + y * a), then the mean's clamp at zero (the shader's one acting clamp,
/// with kMaxRadiance above: a bias, measured by its arm on the card).
inline float roughLobeMean(float carried, float lobe, float blend)
{
    const float m = carried * (1.0f - blend) + lobe * blend;
    return m > 0.0f ? m : 0.0f;
}

} // namespace jahshaka::engine::detail
