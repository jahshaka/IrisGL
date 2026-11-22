#pragma once
// THE ENGINE's GIMODES TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.

namespace jahshaka { namespace engine {

// ---- Global illumination (scene-level; SPECS/PHOTON_SPEC.md is the live
// ---- program doc — GI_SPEC.md and GI_UNIFIED_SPEC.md are its earlier specs,
// ---- written when the program was called Rayon) ----
/// Which GI system lights the scene. Off is the default everywhere — GI must
/// never cost anything unless the author turns it on.
/// INSTANT RADIOSITY IS GONE (PHOTON_SPEC §7 E2 (4), 2026-09-15). It was the
/// `Low` tier's technique: a CPU ray trace from ONE light, planting virtual
/// point lights — so it saw one light, no emissive surface and no area light,
/// re-traced on every light move, and could not feed the irradiance field
/// because it produced no volume to feed it from. Photon's Low tier is two
/// camera-centred voxel cascades at 64^3 with the field on instead, which is
/// cheaper on the frame, sees every light, and is the SAME arm as the tiers
/// above it rather than a second lighting model nobody could reason about.
///
/// The ORDINALS MOVED with it (Vct 2 -> 1, VctPccHybrid 3 -> 2). That is safe
/// and deliberate: every serializer in the tree writes these as STABLE STRINGS
/// and says so at the table (`scenewriter.cpp`, "the enum ints must stay free to
/// be reordered"), and a document that still says `instant_radiosity` reads back
/// as `Vct` — which is what its tier resolves to now.
enum class GiMode {
    Off,
    Vct,                ///< voxel cone tracing over the GI bounds (diffuse + specular GI)
    VctPccHybrid        ///< VCT plus parallax-corrected cubemap probes: probe reflections
                        ///< near geometry, cone-traced reflections far from it
};
/// THE GI CARRIES OCCLUSION (SSAO-DOUBLE-1, 2026-09-29, measured). Every GI mode
/// routes the diffuse through the irradiance field, the cards and the cones, and
/// each carries its own visibility (the probes' depth, the cones' opacity, the
/// cards' traced sun). Screen-space AO is a PROXY for occlusion a renderer did not
/// compute, and it multiplies the FINISHED colour — so over a GI it counts
/// occlusion twice and darkens by a factor that knows nothing of the surface's
/// albedo (a cube on a floor: -11 codes on the contact band at Epic, -13 on a
/// white floor, where the GI alone reads -0.4 / +1.3; spikes/reflect-leak-2).
/// So the chain REFUSES the SSAO passes in every view of a scene whose mode is
/// not Off, whatever the view's PostFxDesc asks (OgreView::chainDesc), and
/// GiStatus::ssaoSuppressed says so. GI Off — a flat ambient with no bounce — is
/// the one honest case for the proxy, and there it runs. THE ONE RULE: the chain,
/// the status and Studio's World row all read it here.
inline bool giCarriesOcclusion(GiMode mode) { return mode != GiMode::Off; }
inline const char *ssaoSuppressedReason() {
    return "the GI carries occlusion (the field's probe depth, the cones' opacity, the cards' "
           "traced sun): screen-space AO over it would count occlusion twice, so the chain "
           "builds without the SSAO passes; SSAO applies with GI off";
}
/// THE PHOTON TIER, as the engine sees it: one row of `giQualityFacts` per value.
/// Epic is a row of its own (D4-PHOTON-TIERS) — it used to ride on High through a
/// separate `epicTier` flag, a second channel for one fact. Epic's facts are High's
/// plus four times the gather's probes; the bounce count is the document's own row.
enum class GiQuality { Low, Medium, High, Epic };

/// A three-state knob whose default answer is "whatever the quality dial says".
/// Used by the hybrid's two expensive probe-capture options
/// (REFLECTIONS_ADOPTION_SPEC.md P3a/P3b): both derive from GiQuality::High by
/// default, and both can be pinned either way independently of it — which is
/// what lets a suite measure ONE of them at a time instead of measuring "High".
enum class GiToggle { Auto, Off, On };

/// WHAT THE SCENE WAS AUTHORED FOR, as far as hardware ray tracing goes
/// (Scene::setRayTracing; iris::RayTracingMode is the document's twin, and the
/// owner's decision is recorded there in full).
///
/// `Auto` traces wherever the machine can and falls back silently; `On` does
/// exactly the same in the renderer and differs only OUTSIDE it (the editor
/// tells the author when the machine falls short — nothing here can conjure
/// ray hardware); `Off` never traces, even on a device that advertises ray
/// queries, so a scene can look and cost the same on every machine.
///
/// The renderer asks ONE question of all this — `Scene::rayTracingResolved()`
/// — and every ray-consuming stage reads that predicate and nothing else.
enum class RayTracingMode { Auto, Off, On };

/// WHY THE REFLECTION-PROBE GRID WAS LAST MARKED STALE (ENGINE_CACHE_POLICY_SPEC
/// §2 P1/P7) — GiStatus::lastStaleReason.
///
/// The probes are a CACHE: captured once, reused every frame, and re-captured
/// only when one of their inputs changes. Every input that can change what a
/// probe would capture marks the grid stale with its reason, and the per-frame
/// update budget spends itself on stale probes only — so a still scene
/// re-captures nothing at all. This names the input, so "why is the sweep
/// running?" is a reading rather than a guess.
///
///   `Rebuild`   a from-scratch build placed a fresh grid (mode, quality, grid,
///               bounds, anything destroyed) — every probe owes a capture
///   `Refresh`   a GI re-solve (the mirror's settle after a drag, or
///               world.refreshGi()) — the voxels moved under the probes
///   `Moved`     geometry the probes capture moved, arrived, left, or was
///               shown, hidden or flagged helper — GI geometry through the
///               movement scan, UNLIT geometry through a probe-only stale that
///               never touches the voxels. A DECAL added, edited, moved or
///               removed is this reason too (it paints a surface the probes
///               capture, and it is never voxelized)
///   `Light`     a light was added, switched on or off, or changed a parameter
///   `Material`  a material parameter or texture used by visible geometry changed
///   `Sky`       the sky or its reflection cubemap changed
///   `Ambient`   the ambient (flat, hemisphere or sky SH) changed
///   `Fog`       the fog description changed
///   `Mobility`  an object's mobility class changed, so it left (or joined) the
///               channel the probes capture — the probes holding its photograph
///               owe one re-capture (Scene::setNodeMovable)
///   `Camera`    the CAMERA moved, and a camera-following cache had to redo work
///               (Photon's cascade chain re-centring — the one reason in this
///               list that is not an edit, which is exactly why it needs its own
///               name: a capture must be able to separate the cost of walking
///               around a scene from the cost of changing it)
///   `None`      nothing has staled the grid since the scene was created
///
/// TIME-VARYING CONTENT IS FROZEN in the probes (REALTIME_REFLECTIONS_SPEC O4,
/// lead decision A, 2026-09-12): a posing rig, a particle system, a clock-driven
/// material or a live/video texture is captured as it was when the grid last
/// re-captured, and stales nothing on its own. SSR and planar reflections show
/// such content live; probes are the static-environment layer.
enum class GiStaleReason { None, Rebuild, Refresh, Moved, Light, Material, Sky, Ambient, Fog, Mobility,
                           Camera };

/// THE MOST CASCADES ANY CHAIN BUILDS — a tier's (giQualityFacts' longest
/// chain) and a scripted one (GiParams::cascadeSet) alike (CASCADE-CAP-1). The
/// PBS pass-texture budget is sized on it (OgreFog.cpp, PhotonPassBinding): a
/// longer chain would claim more set-0 slots than the 64-slot table holds (a
/// fifth anisotropic cascade alone overflows it). world.gi refuses above it.
constexpr int kGiTierMaxCascades = 4;

/// WHAT THIS FRAME MAY PUT OFF (SPECS/OPEN_COVER_SPEC.md §2.1, lane OPEN-COVER-2a).
///
/// Opening or creating a world used to hand ONE frame every first-time cost the
/// world has — the GI arm built from scratch, the probe grid placed, the frame's
/// own texture drain — and that frame is on the UI thread, so the window froze
/// for as long as it took (measured: 1,025 ms on Grand Showroom 2, of which the
/// probe placement alone is 673). The work is not avoidable; doing all of it in
/// one frame is.
///
/// So a caller may say what kind of frame this is, and the engine spreads the
/// first-time work accordingly. THE DEFAULT IS `Complete`, and that is the
/// whole safety argument: a caller that never asks — every offscreen readback,
/// every thumbnail, every pixel suite, the selftest, the VR pump, a script's
/// `editor.frame` — gets exactly the frame it always got, built to completion,
/// with no streaming rule anywhere near it. IN FORCE UNTIL SET AGAIN (Engine::setFramePace), never reset by a frame:
/// a caller that sets `Streaming` gives `Complete` back itself.
/// THE OTHER HALF — "nothing of this world is on screen yet" — is NOT a
/// property of a frame and must not be modelled as one: a load is a stretch of
/// many frames drawn from half a dozen places (the runner's slice boundaries,
/// the cover's inline presents, the shader warm-up's 4x4 target, the project
/// tile's offscreen shot), and a single one of them left at `Complete` builds
/// the whole arm in the middle of the load. That is `Scene::setLoading`, a
/// sticky flag the host raises for the length of the load.
/// WHO ASKED FOR A GI REFRESH (SPECS/OPEN_COVER_SPEC.md §2 A, fix round item 4).
/// `Explicit` is a person or a script — `world.refreshGi()`, a quality change,
/// a suite asserting on the next frame — and is promised the WHOLE answer, so a
/// staged arm build is finished on the spot. `Settle` is the mirror noticing
/// the scene stopped moving; it is worth the cheapest correct step and nothing
/// more, because draining a staged build there would put the whole block back
/// into one frame (~700 ms on Grand Showroom 2) for a drag that ended inside
/// the streaming window.
enum class GiRefreshReason { Explicit, Settle };

}}  // namespace jahshaka::engine
