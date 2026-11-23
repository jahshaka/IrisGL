/**************************************************************************
This file is part of JahshakaVR, VR Authoring Toolkit
http://www.jahshaka.com
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/scenegraph/worldmodes.h"

#include "document/scenegraph/scene.h"
#include "jahshaka/engine/GiModes.h"
#include "jahshaka/engine/GiParams.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "document/properties/valueshapes.h"
#include "core/text.h"
#include "core/math/scalar.h"

namespace worldmodes {

namespace {

/// The planar-reflection budget per tier, shared by the row below and by the
/// resolver beside it (which cannot call rows() — it runs while rows() is being
/// built). Low, Medium, High, Epic.
const int kPlanarTier[4] = { 0, 0, 1, 2 };

/// The row's `get`. iris::Scene::planarReflectionBudget carries -1 for "never
/// set, follow the mode" — the one negative value the field can hold — and the
/// registry's contract is that a row's get() returns the RESOLVED value. Every
/// path that applies a mode writes a concrete number through, so -1 only ever
/// survives in a Custom-mode scene or one loaded from a document written before
/// the feature existed; both resolve to "off", which is also exactly what
/// SceneMirror does with a negative budget.
int planarBudgetOf(const iris::ScenePtr &s)
{
    if (!s) return 0;
    if (s->planarReflectionBudget >= 0) return s->planarReflectionBudget;
    const int m = s->worldMode;
    return (m >= 0 && m <= 3) ? kPlanarTier[m] : 0;
}

/// THE PHOTON TIER TABLE (worldmodes.h's comment is the readable form). Per
/// tier: technique (GiMode ordinal), quality (GiQuality ordinal), ddgi (0/1),
/// bounces (0/1: ON at every tier - PHOTON-7 stage 4: the voxel store converges the bounces by feedback, so there is no
/// count of passes to pick; 0 is the direct store alone, a measurement setting).
/// Owner option (b), 2026-09-09; the fifth column
/// (dynamic probes) was deleted with the feature by lane R2, 2026-09-12.
///
/// ONE SOURCE. The four Photon-tiered rows buildRows() declares take their
/// `tier[]` columns FROM this table (photonColumns below) and the public
/// per-column readers (photonTechnique .. photonBounces) read it too, so
/// there is no second copy to drift — gi.tiers asserts the rows against the
/// readers cell by cell (the review found 13 of the 20 cells were untested
/// while they were hand-copied).
/// The fifth column, probeSize, is the REFLECTION-PROBE CAPTURE SIZE in pixels
/// per cube face (owner, 2026-09-13 Q4: "yes halve it but add it to the world
/// settings"). 0 = follow the engine's quality dial, which is what every tier
/// writes today, and the dial is where the sizes themselves live
/// (OgreGi.cpp buildPcc: 128 at Low, 256 at Medium, 512 at High and Epic —
/// the 2026-09-13 halving of High was REVERSED by the owner on 2026-09-15,
/// ledger §324). A number here instead of 0 would move only scenes authored
/// after the change and would make every scene already saved read as Custom,
/// since its stored 0 would no longer match the tier — so the column stays 0
/// and exists for an AUTHOR to pin a size per scene.
/// THE ORDINALS MOVED WITH INSTANT RADIOSITY (PHOTON_SPEC §7 E2 (4)): the
/// technique column is `iris::GiMode` and that enum is now Off 0 / VCT 1 /
/// the hybrid 2 (techniqueLabel names it: VCT + rays where the scene traces,
/// VCT + probes where it does not). Low is a VOXEL tier now — two camera-centred cascades at
/// 64^3 with the irradiance field on and no probes — which is cheaper on the
/// frame than the CPU ray trace it replaces, sees every light instead of one,
/// and is the same arm as the tiers above it.
/// THE VOXELS ARE ALWAYS THE CAMERA'S CASCADE CHAIN (PHOTON_SPEC §7 E2 (6)); the
/// column that could turn it off, and the single scene-fitted volume it selected,
/// are deleted (D4-PHOTON-TIERS — the owner's law: no fixed GI volume, no "room").
/// THE FIELD COLUMN IS A PROJECTION of the engine's table (D4-PHOTON-TIERS):
/// giQualityFacts(quality).fieldDefault, the same answer GiParams::ddgi's Auto
/// resolves to in the renderer — one source, not two copies.
struct PhotonRow { int technique = 0, quality = 0, bounces = 0, probeSize = 0; };
const PhotonRow kPhotonTable[4] = {
    /* Low    */ { 1, 0, 1, 0 },   // VCT, 2 cascades @ 64^3, the field on, no probes
    /* Medium */ { 1, 1, 1, 0 },   // VCT, the chain at 64^3, DDGI-fed (voxel source)
    /* High   */ { 2, 2, 1, 0 },   // the hybrid (rays where the scene traces, else probes), the chain's High table
    /* Epic   */ { 2, 3, 1, 0 },   // the Epic row (4x the gather's probes)
};
int photonFieldColumn(const PhotonRow &r)
{
    return jahshaka::engine::giQualityFacts(jahshaka::engine::GiQuality(iris::bound(0, r.quality, 3)))
                   .fieldDefault ? 1 : 0;
}
/// The Photon-tiered row ids, in kPhotonTable column order.
const int kPhotonRowCount = 5;
int photonColumn(const PhotonRow &r, int i) {
    switch (i) {
    case 0: return r.technique;
    case 1: return r.quality;
    case 2: return photonFieldColumn(r);
    case 3: return r.bounces;
    default: return r.probeSize;
    }
}
/// Fills a Photon-tiered row's four tier cells from the table's column `column`.
void photonColumns(Row &r, int column)
{
    for (int t = 0; t < 4; ++t) r.tier[t] = photonColumn(kPhotonTable[t], column);
}

/// THE GI CARRIES OCCLUSION (SSAO-DOUBLE-1): the engine's one rule
/// (jahshaka::engine::giCarriesOcclusion) on the scene's technique — the
/// document's iris::GiMode ordinals are the engine's (Off 0, VCT 1, the hybrid 2).
bool ssaoRefusedByGi(const iris::ScenePtr &scene)
{
    return scene && jahshaka::engine::giCarriesOcclusion(
                        jahshaka::engine::GiMode(iris::bound(0, int(scene->giMode), 2)));
}

/// The four tier columns, in order: Low, Medium, High, Epic.
/// Values and reasons come from POST_CHAIN_SPEC.md §9.3, with two documented
/// departures: hardware MSAA is 2x in every tier (the scene under the post chain
/// renders at 1x regardless — see that row — so 2x buys the helpers' and the
/// chain-off scenes' edges for a cost below measurement), and screen-space reflections are DECLARED but not
/// yet served, the same contract shape planar reflections used before its lane
/// landed. A row that silently does nothing is worse than a row that says so.
///
/// EPIC WAS RETUNED (fps audit F6, perf wave 2026-09-06). Three rows moved and
/// the reason is the same in all three: they were the tier's per-pixel and
/// per-frame heavyweights and none of them was carrying its cost in visible
/// quality — full-res 64-tap SSAO -> half res, a 4096 shadow atlas -> 2048,
/// PCF 6x6 -> 4x4. HDR, bloom, SMAA Ultra, VCT GI and the planar budget are
/// UNTOUCHED: Epic is still the tier that turns everything on (SSAO aside: off at
/// every tier since SSAO-DOUBLE-1 — the GI carries the occlusion). Each row carries
/// its own note; the before/after screenshots that justified it are in the
/// wave's report.
// ---- the machine-dependent reflection phrases (STUDIO-CRUD-1 item 6) -------
// After PHOTON-F12-PCC a ray tier builds NO probe grid wherever the scene
// traces, so every row that names a reflection source at High and Epic reads
// its words from these, keyed on the engine's own rayReflections fact.

/// The tiers whose technique column is `technique`, as a phrase
/// ("High and Epic"), read from kPhotonTable.
std::string tiersWithTechnique(int technique)
{
    static const char *names[4] = { "Low", "Medium", "High", "Epic" };
    std::vector<std::string> out;
    for (int t = 0; t < 4; ++t)
        if (kPhotonTable[t].technique == technique) out.push_back(names[t]);
    if (out.size() <= 1) return iris::text::join(out, std::string());
    const std::string last = out.back();
    out.pop_back();
    return iris::text::join(out, ", ") + " and " + last;
}

/// What answers a reflection the screen march cannot, at the tiers the march
/// runs (High and Epic).
std::string reflectionFallback(bool raysResolve)
{
    return raysResolve
        ? "the traced rays, then the voxel cone with the sky as its escape"
        : "the reflection probes and the voxel cone, with the sky as the last "
                         "resort";
}

std::vector<Row> buildRows()
{
    std::vector<Row> out;

    // ---- Rendering ---------------------------------------------------------
    {
        Row r;
        r.id = "msaa";
        r.label = "Anti-Aliasing (MSAA)";
        r.group = "Rendering";
        r.type = RowType::Enum;
        r.options = { { "off", "Off", 1 },
                      { "2x",  "2x",  2 },
                      { "4x",  "4x",  4 },
                      { "8x",  "8x",  8 } };
        // 2x in every tier (owner, 2026-09-15, ledger §339, on DEFAULTS-1's
        // measurement). What the number CAN and CANNOT do: with the post chain
        // on, hardware MSAA either crashes the driver (HDR) or renders black
        // (ambient occlusion) — both reproduced in tests/engine — so the chain
        // renders the SCENE at 1x regardless of what is asked here and SMAA
        // smooths its edges. What a multisampled window still anti-aliases is
        // everything drawn INTO the window outside the chain: the gizmos, the
        // light wires and the helpers (measured: 1x vs 2x at Epic moves only
        // the helper-wire pixels), and the whole scene at Low, which runs with
        // the chain off and had no anti-aliasing at all at 1x. 2x costs below
        // the rig's noise on the target GPU; 4x/8x stay a user's choice.
        r.tier[0] = 2; r.tier[1] = 2; r.tier[2] = 2; r.tier[3] = 2;
        r.cost = "Hardware edge smoothing. Costs render-target memory and "
                                "bandwidth in proportion to the sample count, and the driver may "
                                "clamp the request. While HDR or ambient occlusion is on the "
                                "scene itself renders at 1x and SMAA smooths it; the gizmos, "
                                "light wires and helpers still get this sample count.";
        r.get = [](const iris::ScenePtr &s) { return s->antiAliasing; };
        r.set = [](const iris::ScenePtr &s, int v) { s->antiAliasing = v; };
        out.push_back(r);
    }

    // ---- Post-processing chain (POST_CHAIN_SPEC.md phases 3-7) --------------
    // These rows appear only once the renderer can actually serve them: a row
    // that silently does nothing is worse than no row at all.
    {
        Row r;
        r.id = "hdr";
        r.label = "HDR + Filmic Tonemap";
        r.group = "Rendering";
        r.type = RowType::Bool;
        r.tier[0] = 0; r.tier[1] = 1; r.tier[2] = 1; r.tier[3] = 1;
        r.cost = "Renders into a floating-point buffer and grades it with a film "
                                "curve at the scene's exposure (the Exposure Mode row below says "
                                "whether that is a number or a measurement). The bloom work is at "
                                "a fixed size, so only the final pass scales with the window. Off "
                                "at Low purely to save the buffer.";
        r.get = [](const iris::ScenePtr &s) { return s->hdrEnabled ? 1 : 0; };
        r.set = [](const iris::ScenePtr &s, int v) { s->hdrEnabled = v != 0; };
        out.push_back(r);
    }
    {
        // EXPOSURE MODE (EXPOSURE-1, 2026-09-17; RENDER AUDIT A1/A3). A
        // TierSpace::None row: how a scene is exposed is an ART decision, so no
        // World Mode column writes it and a tier switch never regrades anybody's
        // picture — the same rule the continuous parameters have always had.
        // It is a Row rather than a ParamRow because it is a CHOICE of two
        // things, not a number, and because the panel and world.override then
        // get it for free.
        Row r;
        r.id = "exposureMode";
        r.label = "Exposure Mode";
        r.group = "Rendering";
        r.type = RowType::Enum;
        r.tierSpace = TierSpace::None;
        r.options = { { "manual", "Manual",
                        int(iris::ExposureMode::Manual) },
                      { "auto", "Auto",
                        int(iris::ExposureMode::Auto) } };
        r.cost = "MANUAL (the default) develops the picture at the Exposure below "
                                "and measures nothing: it is exact on the first frame, it is five "
                                "passes and four textures cheaper than the meter, and it is the "
                                "same grade a thumbnail or a screenshot of this world gets. AUTO "
                                "meters the frame and adapts within the window — which is what a "
                                "camera does, and what makes a white surface filling the frame "
                                "darken everything else by about two and a half stops. Use Auto "
                                "when the light in the shot changes and you want the picture to "
                                "follow it.";
        r.get = [](const iris::ScenePtr &s) { return int(s->exposureMode); };
        r.set = [](const iris::ScenePtr &s, int v) {
            s->exposureMode = v == int(iris::ExposureMode::Auto) ? iris::ExposureMode::Auto
                                                                 : iris::ExposureMode::Manual;
        };
        out.push_back(r);
    }
    {
        // METERING PATTERN (EXPOSURE-2). Beside Exposure Mode, and a
        // TierSpace::None row for the same reason: how a scene is metered is an
        // ART decision, so no quality tier writes it. Shown only under Auto —
        // it is the one thing that would be a LIE under Manual, which measures
        // nothing (the same rule the auto window's pair has).
        Row r;
        r.id = "exposureMetering";
        r.label = "Metering";
        r.group = "Rendering";
        r.type = RowType::Enum;
        r.tierSpace = TierSpace::None;
        r.options = { { "average", "Average",
                        int(iris::ExposureMetering::Average) },
                      { "centreWeighted", "Centre Weighted",
                        int(iris::ExposureMetering::CentreWeighted) },
                      { "spot", "Spot",
                        int(iris::ExposureMetering::Spot) } };
        r.visible = [](const iris::ScenePtr &s) {
            return s->exposureMode == iris::ExposureMode::Auto;
        };
        r.cost = "WHERE the automatic exposure looks. AVERAGE weighs the whole "
                                "frame equally, so a bright sky or a white floor filling most "
                                "of it takes everything else down. CENTRE WEIGHTED (the "
                                "default, and what a camera does) puts about 41% of the "
                                "sensitivity in the middle eleventh of the picture and 83% "
                                "inside the circle that fits its height, with the corners "
                                "still counting for a twentieth. SPOT reads a disc of about "
                                "2.5% of the frame at the centre and nothing else — point it "
                                "at the subject. It costs nothing either way: the pattern is a "
                                "per-pixel weight in the meter's own compute pass.";
        r.get = [](const iris::ScenePtr &s) { return int(s->exposureMetering); };
        r.set = [](const iris::ScenePtr &s, int v) {
            s->exposureMetering = v == int(iris::ExposureMetering::Average)
                                      ? iris::ExposureMetering::Average
                                  : v == int(iris::ExposureMetering::Spot)
                                      ? iris::ExposureMetering::Spot
                                      : iris::ExposureMetering::CentreWeighted;
        };
        out.push_back(r);
    }
    {
        Row r;
        r.id = "bloom";
        r.label = "Bloom";
        r.group = "Rendering";
        r.type = RowType::Bool;
        r.tier[0] = 0; r.tier[1] = 1; r.tier[2] = 1; r.tier[3] = 1;
        r.cost = "Highlight glow, spreading evenly in every direction: a ladder of "
                                "six half-size copies of the picture, from half the view down to a "
                                "sixty-fourth. A fraction of a millisecond, and it needs HDR, and "
                                "does nothing without it.";
        r.get = [](const iris::ScenePtr &s) { return s->bloomEnabled ? 1 : 0; };
        r.set = [](const iris::ScenePtr &s, int v) { s->bloomEnabled = v != 0; };
        out.push_back(r);
    }
    {
        Row r;
        r.id = "ssao";
        r.label = "Ambient Occlusion";
        r.group = "Rendering";
        r.type = RowType::Enum;
        r.options = { { "off",  "Off",        0 },
                      { "half", "Half Res",   1 },
                      { "full", "Full Res",   2 } };
        // OFF AT EVERY TIER, BECAUSE EVERY TIER IS A GI TIER (SSAO-DOUBLE-1). The
        // GI carries its own visibility, and SSAO multiplies the FINISHED colour:
        // over a GI it counts occlusion twice and ignores the surface's albedo
        // (a cube's contact band -11 codes at Epic, -13 on a white floor, where
        // the GI alone reads -0.4 / +1.3; spikes/reflect-leak-2). The ENGINE
        // refuses the passes wherever the GI is on (giCarriesOcclusion) whatever
        // this row says; the row follows it — a pin is kept, and the entries
        // name the refusal (optionLabelAt), because the settings' truth is what
        // renders. With Photon off the proxy is honest and a pin runs it.
        r.tier[0] = 0; r.tier[1] = 0; r.tier[2] = 0; r.tier[3] = 0;
        r.optionLabelAt = [](int value, const iris::ScenePtr &scene, bool) {
            const std::string plain = value == 0 ? "Off"
                                : value == 1 ? "Half Res"
                                             : "Full Res";
            if (!ssaoRefusedByGi(scene)) return plain;
            return value == 0
                ? "Off (the GI carries occlusion; SSAO applies with GI off)"
                : plain + " (suppressed: the GI carries occlusion; SSAO applies "
                                         "with GI off)";
        };
        r.cost = "Screen-space contact shadowing in creases and corners — a PROXY "
                                "for occlusion a renderer did not compute, multiplied into the "
                                "finished picture. Photon computes it: wherever global "
                                "illumination is on, its probes, cones and cards carry their own "
                                "visibility, so the renderer refuses this pass (a second "
                                "occlusion would darken twice, and without regard to the "
                                "surface's colour) and every tier leaves it off. With Photon off "
                                "it applies: 64 samples per pixel, fixed by the shader — the only "
                                "lever is the buffer resolution (Full Res is four times the "
                                "samples of Half Res for a low-frequency signal).";
        // One row, two backing fields: the enable flag and the buffer scale.
        r.get = [](const iris::ScenePtr &s) {
            if (!s->ssaoEnabled) return 0;
            return s->ssaoScale >= 0.99f ? 2 : 1;
        };
        r.set = [](const iris::ScenePtr &s, int v) {
            s->ssaoEnabled = v != 0;
            if (v) s->ssaoScale = v >= 2 ? 1.0f : 0.5f;
        };
        out.push_back(r);
    }
    {
        Row r;
        r.id = "smaa";
        r.label = "SMAA (post AA)";
        r.group = "Rendering";
        r.type = RowType::Enum;
        r.options = { { "off",    "Off",    -1 },
                      { "low",    "Low",     0 },
                      { "medium", "Medium",  1 },
                      { "high",   "High",    2 },
                      { "ultra",  "Ultra",   3 } };
        // SMAA is the chain's anti-aliasing at every tier that has a chain.
        // POST_CHAIN_SPEC §9.3 proposed MSAA at High/Epic and SMAA only at
        // Medium; that split is not available (see the MSAA row above), so the
        // preset simply climbs with the tier instead. Low has no chain and no AA.
        r.tier[0] = -1; r.tier[1] = 1; r.tier[2] = 2; r.tier[3] = 3;
        r.cost = "Edge anti-aliasing after tonemapping, at a fixed cost per pixel "
                                "instead of MSAA's per-sample memory. Every preset is compiled "
                                "when the app starts: changing it compiles nothing.";
        r.get = [](const iris::ScenePtr &s) { return s->smaaPreset; };
        r.set = [](const iris::ScenePtr &s, int v) { s->smaaPreset = v; };
        out.push_back(r);
    }
    {
        // SSR SHIPPED (POST_CHAIN_SPEC §8 phase 6; OgreChain.cpp's SSR block and
        // irisgl/engine/media/Hlms/Jahshaka/JahSsr*). The row is served: a
        // depth/normal/roughness prepass feeds a ray march that HlmsPbs composites
        // into the specular environment term itself (`hlms_use_ssr`), so where the
        // march is confident the screen replaces the probe/sky answer and where it
        // is not the pixel is byte-for-byte what it is today. Pixel gate:
        // `ssr.engine`.
        //
        // THE TIERS ARE THE MEASUREMENT, not a guess. On the RTX 4080 SUPER
        // baseline, offscreen 1x, a 62-object fixture (tests/ssr with
        // JAH_SSR_BENCH=1):
        //     1920x1080   passthrough 0.41 ms   half-res +0.07..0.10   full-res +0.11..0.13
        //     3840x2160   passthrough 0.44 ms   half-res +0.11         full-res +0.54
        //     1920x1080, 602 objects              half-res +0.13        full-res +0.15
        // Two things follow. (a) The ray march is pixel-bound and half resolution
        // really is a quarter of it — at 4K the two rows separate by 5x, which is
        // why the quality row exists at all. (b) At 1080p the march is BELOW the
        // measurement floor and what is actually being paid is the second scene
        // traversal, which grows with object count, not with resolution. That is
        // the reason this row stays off below High: the cost a heavy world pays is
        // CPU submission, and this renderer is CPU-bound long before it is
        // pixel-bound.
        Row r;
        r.id = "ssr";
        r.label = "Screen-Space Reflections";
        r.group = "Reflections";
        r.type = RowType::Enum;
        r.options = { { "off",  "Off",           0 },
                      { "half", "Half-Res Rays", 1 },
                      { "hq",   "Full-Res Rays", 2 } };
        r.tier[0] = 0; r.tier[1] = 0; r.tier[2] = 1; r.tier[3] = 2;
        // AT A RAY TIER THE RAYS ARE THE REFLECTION (D4-PHOTON-TIERS): the row
        // offers exactly two states — "Off", honoured at every tier (no trace, no
        // march), and "Traced" standing for every other value: the march's
        // quality means nothing there, the trace runs at the tier's own
        // resolution (giQualityFacts reflectTrace, this row's own High/Epic
        // columns; SceneMirror feeds it). Choosing Traced DROPS the pin (the
        // tier decides again; applyComboItem) — the way back from Off without
        // resetting every other pin. Elsewhere the row is the screen march.
        r.comboItemsAt = [](const iris::ScenePtr &scene, bool sceneTracesRays) {
            std::vector<ComboItem> items;
            if (reflectionsTraced(scene, sceneTracesRays)) {
                items.push_back({ "off", "Off", 0, false, {} });
                items.push_back({ "traced", "Traced", 1, true, { 2 } });
            } else {
                items.push_back({ "off",  "Off",           0, false, {} });
                items.push_back({ "half", "Half-Res Rays", 1, false, {} });
                items.push_back({ "hq",   "Full-Res Rays", 2, false, {} });
            }
            return items;
        };
        r.costAt = [](bool sceneTracesRays) {
            const bool rays = tierRaysResolve(PhotonTier::High, sceneTracesRays);
            return iris::text::cat("Reflections of things that MOVE — the one gap a baked capture structurally cannot fill, and the only reflection source that needs no capture at all. Costs a second traversal of the scene (a depth/normal/roughness prepass) on top of the ray march, so it never appears below High. Only reflects what is ON SCREEN: reflections fade out at the frame's edges and on rough surfaces, and fall back to ", reflectionFallback(rays), " wherever they do.", rays ? iris::text::cat(" At ", tiersWithTechnique(2), ", where this scene traces rays, the RAYS are the reflection: this row reads Traced and the renderer traces at the tier's own resolution (High one ray per 2x2 pixels, Epic every pixel) whatever it is set to.")
                          : std::string());
        };
        r.available = true;
        r.get = [](const iris::ScenePtr &s) { return s->ssrMode; };
        r.set = [](const iris::ScenePtr &s, int v) { s->ssrMode = v; };
        out.push_back(r);
    }
    {
        // ...AND THE TRACE'S RESOLUTION (owner section 1804): Auto is the tier's own column.
        Row r;
        r.id = "reflectionResolution";
        r.label = "Reflection Resolution";
        r.group = "Reflections";
        r.type = RowType::Enum;
        r.tierSpace = TierSpace::None;
        r.options = { { "auto", "Auto", 0 },
                      { "full", "Full", 1 },
                      { "half", "Half", 2 } };
        r.cost = "The ray tier's reflection resolution. AUTO (the default) is the tier's: High one "
                                "ray per 2x2 pixels, Epic every pixel. FULL traces every pixel at any ray tier (four "
                                "times High's rays); HALF one per 2x2 block at Epic too.";
        r.get = [](const iris::ScenePtr &s) { return iris::bound(0, s->reflectionResolution, 2); };
        r.set = [](const iris::ScenePtr &s, int v) { s->reflectionResolution = iris::bound(0, v, 2); };
        out.push_back(r);
    }
    {
        // THE SURFACE CACHE (PHOTON-CARDS-2): a reflection ray's hit reads the
        // hit surface's card first, so the cache is worth its capture exactly
        // where the ray tier runs — and the ray tier rides the SSR row above
        // (off at Low and Medium). So the column is OFF there and AUTO at High
        // and Epic (the engine resolves Auto against the machine's rays: a GPU
        // without them captures nothing even at Epic).
        Row r;
        r.id = "giCards";
        r.label = "Surface Cache (Hit Lighting)";
        r.group = "Reflections";
        r.type = RowType::Enum;
        r.options = { { "off",  "Off",  0 },
                      { "auto", "Auto", -1 } };
        r.tier[0] = 0; r.tier[1] = 0; r.tier[2] = -1; r.tier[3] = -1;
        r.cost = "Photographs of the surfaces around the camera, lit, that a "
                                "traced reflection reads at the point it hits — a mirror shows a "
                                "surface's own lighting instead of the voxels' 15 cm cells. Costs "
                                "a 100 MB atlas and a few card captures a frame; Auto turns it on "
                                "only where reflection rays run, and a wide glossy lobe keeps "
                                "reading the voxels (a card texel would speckle there).";
        r.get = [](const iris::ScenePtr &s) { return s->giCards; };
        r.set = [](const iris::ScenePtr &s, int v) { s->giCards = v; };
        out.push_back(r);
    }
    {
        // THE ROUGHNESS CUTOFF (PHOTON_SPEC §7 R5; owner, ledger §426). It
        // belongs beside the reflection rows because it is the one number that
        // says where reflections stop being worth computing per pixel at all —
        // the same lobe the row above marches and the ray tier traces.
        //
        // WHY IT IS A PROJECT'S NUMBER AND NOT THE RENDERER'S: above the cutoff
        // a reflection is a wide lobe and a reflection probe's prefiltered
        // photograph IS a good integral of it, so a ray or a march per pixel
        // buys a blurrier answer for the same cost. Where that crossover falls
        // depends on the CONTENT — a polished gallery and a weathered street
        // stop being worth tracing at different roughnesses — and no constant
        // can be right for both.
        //
        // The same value in EVERY tier column: it is not a quality trade (the
        // cost of a tier is the resolution and whether rays run at all, both
        // rows of their own), it is a description of the scene's surfaces. A
        // tier change must not silently re-author it.
        Row r;
        r.id = "reflectionRoughnessCutoff";
        r.label = "Roughness Cutoff";
        r.group = "Reflections";
        r.type = RowType::Int;
        r.minValue = 5;
        r.maxValue = 100;
        r.tier[0] = 40; r.tier[1] = 40; r.tier[2] = 40; r.tier[3] = 40;
        // WHAT THIS ROW DRIVES, and it is now BOTH sources (lane SSR-3). The
        // screen-space march used to keep a cutoff of its own
        // (`PostFxDesc::ssrRoughnessCutoff`, 0.35) that nothing in the document
        // could write — and that the marcher compared against the G-buffer's GGX
        // ALPHA without ever square-rooting it, so the band the frame applied
        // was perceptual 0.581, a number nobody chose and nobody could read.
        // That second cutoff is DELETED and the march takes this dial, so the
        // tooltip may say the whole sentence again.
        r.costAt = [](bool sceneTracesRays) {
            const bool rays = tierRaysResolve(PhotonTier::High, sceneTracesRays);
            return iris::text::cat("How rough a surface may be and still have its reflections computed per pixel — MARCHED in screen space and, on a machine with ray-tracing hardware, TRACED — in per cent. Below it the screen answers what it can see", rays ? " and a traced ray answers the rest" : std::string(), "; above it ", rays ? "the voxel cone's blurred reflection (the sky where "
                                           "it escapes)"
                          : "the reflection probes' own blurred photograph", " answers, which for a rough surface is both cheaper and closer to the truth. The change is feathered: both the screen's march and the ray fade out over the same 10 points of this scale below the value, so a surface whose roughness varies across it has no seam in it. Raising it spends rays and marches on surfaces that will look much the same either way; lowering it hands more of the picture to ", rays ? "the voxel cone" : "the probes", ".");
        };
        r.available = true;
        r.get = [](const iris::ScenePtr &s) { return s->reflectionRoughnessCutoff; };
        r.set = [](const iris::ScenePtr &s, int v) { s->reflectionRoughnessCutoff = v; };
        out.push_back(r);
    }
    {
        Row r;
        r.id = "refractions";
        r.label = "Refractive Glass";
        r.group = "Rendering";
        r.type = RowType::Enum;
        r.options = { { "off",  "Off",  0 },
                      { "auto", "Auto", 1 },
                      { "on",   "On",   2 } };
        r.tier[0] = 0; r.tier[1] = 0; r.tier[2] = 1; r.tier[3] = 1;
        r.cost = "Glass that BENDS what is behind it. Auto is the honest setting: "
                                "the extra scene pass and full-screen copy only enter the frame "
                                "while the scene actually contains a refractive material, so it "
                                "costs nothing until you make one.";
        r.get = [](const iris::ScenePtr &s) { return s->refractionsMode; };
        r.set = [](const iris::ScenePtr &s, int v) { s->refractionsMode = v; };
        out.push_back(r);
    }
    {
        // DISTORTION (POST_LOOKS_SPEC.md §5.3). MACHINERY, so it IS a tiered row
        // — unlike the looks stack, which is an art choice and deliberately
        // tierless (§7 R10): a mode switch must never silently drop somebody's
        // look, but it may reasonably drop a heat-haze pass at Low.
        Row r;
        r.id = "distortion";
        r.label = "Distortion";
        r.group = "Rendering";
        r.type = RowType::Enum;
        r.options = { { "off",  "Off",  0 },
                      { "auto", "Auto", 1 },
                      { "on",   "On",   2 } };
        r.tier[0] = 0; r.tier[1] = 1; r.tier[2] = 1; r.tier[3] = 1;
        r.cost = "Objects that WARP what is behind them — heat haze, blast "
                                "waves, shock rings. Give a material the Distortion shading "
                                "model and its Normal Map becomes the displacement. Auto is "
                                "the honest setting: the extra target and its two passes only "
                                "enter the frame while the scene actually holds such a "
                                "material, so it costs nothing until you make one.";
        r.get = [](const iris::ScenePtr &s) { return s->distortionMode; };
        r.set = [](const iris::ScenePtr &s, int v) { s->distortionMode = v; };
        out.push_back(r);
    }

    {
        // HIDE THE DEFAULT FLOOR IN THE PLAYER (owner, 2026-09-18,
        // VR_INPUT_SPEC §16 row 6: "hide the floor in the Player" — a PROJECT
        // SETTING). A TierSpace::None row, for the reason Exposure Mode gives
        // one file above: no quality tier may write it. It is not a scalability
        // question at all — it is a statement about the finished thing, and a
        // mode switch that silently put somebody's editor floor back into their
        // Player would be the same defect as a mode switch that regraded their
        // picture.
        //
        // IN THE "Rendering" GROUP rather than a new "Player" section, and that
        // is a deliberate small choice: the World tab's sections are counted by
        // ui.properties_filter and a section for one boolean is a worse answer
        // than a row among the other scene-wide switches.
        Row r;
        r.id = "playerHidesFloor";
        r.label = "Hide Floor in Player";
        r.group = "Rendering";
        r.type = RowType::Bool;
        r.tierSpace = TierSpace::None;
        r.cost = "Leaves the app's own checkered default floor — and the horizon "
                                "plane that extends it — OUT of the Player, while the editor "
                                "keeps it. For a scene that stands on its own level, terrain or "
                                "nothing at all: the default floor is where the grid is legible "
                                "and what a dropped object lands on while you build, and it is "
                                "not part of the finished thing. It hides nothing you authored "
                                "and deletes nothing: the floor is still in the outliner, with "
                                "its material and its physics, and it comes back the moment the "
                                "Player stops. Costs nothing either way.";
        r.get = [](const iris::ScenePtr &s) { return s->playerHidesFloor ? 1 : 0; };
        r.set = [](const iris::ScenePtr &s, int v) { s->playerHidesFloor = v != 0; };
        out.push_back(r);
    }
    {
        // THE VIEW'S PIXEL-ERROR THRESHOLD (ATOM-3 stage 1): the one lever of the level rule
        // that is taste rather than arithmetic (the rule itself is measured — ATOM-2 stage 2
        // matched Unreal's to five digits). At 1 px nothing a cut drops can be seen; at 2-4 px
        // the small parts of a far object drop the way a classic LOD does, and the drawn
        // triangles fall with the tier. The numbers are the lead's (2026-10-19); whether 2 px
        // shows on a moving silhouette is the owner's visual call at the smoke (atom.dolly_gate
        // prints the per-step delta at 2 and 3). Every view takes it (the Atom cut and the
        // stock chain alike); shadow maps keep their texel. The selftest scenes run Epic = 1.
        Row r;
        r.id = "atomPixelError";
        r.label = "Geometry Detail (pixel error)";
        r.group = "Rendering";
        r.type = RowType::Int;
        r.minValue = 1; r.maxValue = 8;
        r.tier[0] = 4; r.tier[1] = 3; r.tier[2] = 2; r.tier[3] = 1;
        r.cost = "How many pixels of geometric error a view may show before it "
                                "draws a finer level of a mesh: 1 is the most detail (no "
                                "simplification can be seen), 2-4 let far objects drop small "
                                "parts sooner and draw fewer triangles. Shadow maps are not "
                                "affected (their resolution is their own setting). Costs "
                                "nothing to change: no shader rebuilds.";
        r.get = [](const iris::ScenePtr &s) { return s->atomPixelError; };
        r.set = [](const iris::ScenePtr &s, int v) { s->atomPixelError = iris::bound(1, v, 8); };
        out.push_back(r);
    }

    // ---- Shadows -----------------------------------------------------------
    {
        Row r;
        r.id = "shadowResolution";
        r.label = "Shadow Map Size";
        r.group = "Shadows";
        r.type = RowType::Enum;
        r.options = { { "auto", "Auto", 0 },
                      { "512",  "512",  512 },
                      { "1024", "1024", 1024 },
                      { "2048", "2048", 2048 },
                      { "4096", "4096", 4096 } };
        // EPIC IS 2048, NOT 4096 (fps audit F6). The atlas is R x 3.5R at 32-bit
        // depth, so 4096 is ~235 MB of VRAM *and* four times the shadow-pass
        // fill of 2048 — the whole caster set is rasterised into it every
        // frame. 2048 is what High already used and what the editor's typical
        // scene scale actually resolves; the row is still there for anyone who
        // wants to pin 4096 on a static shot.
        r.tier[0] = 512; r.tier[1] = 1024; r.tier[2] = 2048; r.tier[3] = 2048;
        r.cost = "One shadow atlas for the whole scene, R wide by 3.5R tall at "
                                "32-bit depth: 512 costs ~3.6 MB, 1024 ~14 MB, 2048 ~59 MB, "
                                "4096 ~235 MB of VRAM — and four times 2048's rasterisation "
                                "work every frame, which is why no tier asks for it.";
        r.get = [](const iris::ScenePtr &s) { return s->shadowResolution; };
        r.set = [](const iris::ScenePtr &s, int v) { s->shadowResolution = v; };
        out.push_back(r);
    }
    {
        Row r;
        r.id = "shadowMapBudget";
        r.label = "Shadow Map Budget";
        r.group = "Shadows";
        r.type = RowType::Enum;
        r.options = { { "auto", "Auto", 0 },
                      { "2",    "2 lights",  2 },
                      { "4",    "4 lights",  4 },
                      { "8",    "8 lights",  8 },
                      { "16",   "16 lights", 16 } };
        // THE DEFECT THIS ROW EXISTS FOR (SHADOW_TOOLING_SPEC.md §1): the atlas
        // used to hold exactly TWO point/spot shadow maps, forever, and Ogre
        // fills them with the casters closest to the camera and drops the rest
        // without a word. The shipped Showroom has three shadow-casting lamps;
        // one of them has never cast a shadow, and which one changed as the
        // camera moved.
        //
        // TIERS 2/4/8/8 (owner decision D1). Low keeps today's two; Medium's
        // four covers the ordinary "a lamp in each corner" room; High and Epic
        // sit at eight, which is where the shadow-pass cost — six cube faces
        // plus a copy for EVERY mapped point light — starts to be the thing
        // you are paying for rather than the atlas.
        r.tier[0] = 2; r.tier[1] = 4; r.tier[2] = 8; r.tier[3] = 8;
        r.cost = "A ceiling, not an allocation: the engine grows the atlas in "
                                "steps (2, 4, 8, 16) to fit the scene's shadow-casting point and "
                                "spot lights, and never shrinks it again in one session. Each "
                                "focused map costs R x R of the atlas (at 2048: ~17 MB), so the "
                                "view's atlas runs ~56 MB at 2 lamps, ~88 at 4 and ~160 at 8 — "
                                "and every planar mirror keeps its own half-resolution copy "
                                "(~38 MB at 8) and every shadowed reflection probe a "
                                "quarter-resolution one (~5.5 MB). A point lamp's map is six "
                                "cube-face renders plus a copy, but only in a frame where "
                                "something in its reach changed: a still scene re-renders none "
                                "of them. Empty maps cost no shaders. Lights beyond the budget "
                                "still light the scene; they simply cast no shadow.";
        r.get = [](const iris::ScenePtr &s) { return s->shadowMapBudget; };
        r.set = [](const iris::ScenePtr &s, int v) { s->shadowMapBudget = v; };
        out.push_back(r);
    }
    {
        Row r;
        r.id = "shadowFilter";
        r.label = "Shadow Softness";
        r.group = "Shadows";
        r.type = RowType::Enum;
        r.options = { { "auto",     "Auto",      -1 },
                      { "hard",     "Hard",       0 },
                      { "soft",     "Soft",       1 },
                      { "verysoft", "Very Soft",  2 } };
        // EPIC IS SOFT, NOT VERY SOFT — fps audit F6. THE REAL TAP COUNTS on
        // this engine pin, read from the shader rather than assumed (they were
        // quoted as 16 vs 36 here for a year, which is the arithmetic of the
        // filter's NAME and not of its loop): OgreHlmsPbs.cpp sets pcf /
        // pcf_iterations and ShadowMapping_piece_ps.any runs them, so Hard =
        // PCF_2x2 = ONE gather-based sample, Soft = PCF_4x4 = 9 taps, Very Soft
        // = PCF_6x6 = 25 taps — per shadowed light, per shaded pixel.
        r.tier[0] = 0; r.tier[1] = 1; r.tier[2] = 1; r.tier[3] = 1;
        r.cost = "PCF filter width for every shadowed light: Hard (2x2) is 1 "
                                "hardware gather, Soft (4x4) is 9 taps and Very Soft (6x6) is "
                                "25 — per shadowed light, on every pixel it lights, which is why "
                                "no tier asks for Very Soft. The sun costs this three times over, "
                                "once per cascade. Auto uses the softest quality any light in the "
                                "scene asked for. Takes effect next frame; no rebuild.";
        r.get = [](const iris::ScenePtr &s) { return s->shadowFilterTier; };
        r.set = [](const iris::ScenePtr &s, int v) { s->shadowFilterTier = v; };
        out.push_back(r);
    }

    // ---- Global illumination = PHOTON (GI_UNIFIED_SPEC.md §2) ---------------
    // ONE row is the dial the World Mode drives; the five below it are the
    // machinery that dial consumes, and they are Photon-tiered (Row::tierSpace) — resolved by
    // the PHOTON tier, not by the World Mode, because two dials must never own
    // one backing field. THE TABLE ITSELF is kPhotonTable above; each row's
    // tier[] columns are READ from it (photonColumns), never copied.
    {
        Row r;
        r.id = "photon";
        r.label = "Photon (realtime GI)";
        r.group = "Global Illumination";
        r.type = RowType::Enum;
        r.options = { { "off",    "Off",    0 },
                      { "low",    "Low",    1 },
                      { "medium", "Medium", 2 },
                      { "high",   "High",   3 },
                      { "epic",   "Epic",   4 } };
        // THE IDENTITY (owner, 2026-10-09; WORLD-MODE-1): each World Mode runs Photon at the
        // same name. Photon Off is reachable only from Photon's own switch, and a Photon tier
        // that differs from the column makes the World Mode read Custom (worldmodes::mode).
        r.tier[0] = 1; r.tier[1] = 2; r.tier[2] = 3; r.tier[3] = 4;
        // GENERATED from the two tables (photonTierSummary): this text used to
        // say "Medium voxelizes at twice the resolution" while both tiers were
        // 64 (render audit A5).
        r.cost = "Photon — realtime global illumination: light that bounces off "
                                "surfaces and colours everything it lands on, recomputed live "
                                "instead of baked. Each World Mode runs Photon at the same name. "
                                "What each tier actually runs — " +
                 photonTierSummary() +
                 " Every knob a tier sets is still reachable one by one under "
                                "Advanced, and anything you set there stays set.";
        r.get = [](const iris::ScenePtr &s) {
            if (!s || s->giMode == iris::GiMode::OFF) return 0;
            return iris::bound(0, s->giTier, 3) + 1;
        };
        r.set = [](const iris::ScenePtr &s, int v) {
            if (!s) return;
            const bool on = v > 0;
            const PhotonTier t = on ? PhotonTier(iris::bound(0, v - 1, 3)) : photonTier(s);
            setPhoton(s, on, t);
        };
        out.push_back(r);
    }
    {
        Row r;
        r.id = "giMode";
        r.label = "Photon Technique";
        r.group = "Global Illumination";
        r.type = RowType::Enum;
        r.tierSpace = TierSpace::Photon;
        // The NAMES are techniqueLabel's (the hybrid is "VCT + rays" where the
        // scene traces, "VCT + probes" where it does not); the stored label is
        // the no-engine answer, and every surface reads optionLabel().
        r.options = { { "off",            techniqueLabel(0, false), 0 },
                      { "vct",            techniqueLabel(1, false), 1 },
                      { "vct_pcc_hybrid", techniqueLabel(2, false), 2 } };
        r.optionLabelAt = [](int value, const iris::ScenePtr &scene, bool sceneTracesRays) {
            return techniqueLabel(value, probeGridByRays(scene, sceneTracesRays));
        };
        // PHOTON columns (Low, Medium, High, Epic) — not world-mode ones —
        // read from kPhotonTable. Low and Medium are VCT and differ in the
        // voxel resolution; the top two are the hybrid and differ in the
        // giBounces row below, not here.
        photonColumns(r, 0);
        r.costAt = [](bool sceneTracesRays) {
            const bool rays = tierRaysResolve(PhotonTier::High, sceneTracesRays);
            return iris::text::cat("Which technique Photon uses. VCT re-voxelizes on geometry edits (editing latency, not frame time) and lights from everything. ", rays
                         ? iris::text::cat(techniqueLabel(2, true), " is where the scene traces its reflections: the screen march and the traced rays are the sharp reflections, the voxel cone and the sky behind them, and no reflection-probe grid is built.")
                         : iris::text::cat(techniqueLabel(2, false), " adds sharp reflections near geometry — six renders per reflection probe on every re-solve (18 probes by default), HDR and shadowed at High quality."), " Normally the Photon quality tier picks this (", tiersWithTechnique(1), " ", techniqueLabel(1, false), ", ", tiersWithTechnique(2), " ", techniqueLabel(2, rays), "); setting it here PINS it.");
        };
        r.get = [](const iris::ScenePtr &s) { return int(s->giMode); };
        r.set = [](const iris::ScenePtr &s, int v) { s->giMode = iris::GiMode(v); };
        out.push_back(r);
    }
    {
        Row r;
        r.id = "giQuality";
        r.label = "Photon Voxel/Probe Quality";
        r.group = "Global Illumination";
        r.type = RowType::Enum;
        r.tierSpace = TierSpace::Photon;
        r.options = { { "low",    "Low",    0 },
                      { "medium", "Medium", 1 },
                      { "high",   "High",   2 },
                      { "epic",   "Epic",   3 } };
        photonColumns(r, 1);
        // GENERATED: the resolutions and probe sizes are the ENGINE's
        // (giQualityFacts), per quality row (render audit A5).
        {
            using jahshaka::engine::GiQuality;
            using jahshaka::engine::giQualityFacts;
            r.cost = iris::text::cat("Ray/voxel budget: this dial picks the camera's cascade chain — Low ", photonTierVoxelPhrase(PhotonTier::Low), " voxels per axis, Medium ", photonTierVoxelPhrase(PhotonTier::Medium), ", High ", photonTierVoxelPhrase(PhotonTier::High), ", Epic ", photonTierVoxelPhrase(PhotonTier::Epic), " (Epic adds four times the gather's probes). It also sets the reflection probe's cube face: ", giQualityFacts(GiQuality::Low).probeFaceSize, " / ", giQualityFacts(GiQuality::Medium).probeFaceSize, " / ", giQualityFacts(GiQuality::High).probeFaceSize, " / ", giQualityFacts(GiQuality::Epic).probeFaceSize, " pixels");
        }
        r.costAt = [text = r.cost](bool sceneTracesRays) {
            const bool rays = tierRaysResolve(PhotonTier::High, sceneTracesRays);
            return text +
                   (rays ? iris::text::cat(". High traces its reflections here, so no probe grid is built for that face size to size unless the technique is pinned to ", techniqueLabel(2, false), " or the scene's Ray Tracing row is off; where one is built, High ALSO turns on HDR and shadowed probe captures")
                         : iris::text::cat(". Under ", techniqueLabel(2, false), " High ALSO turns on HDR and shadowed probe captures")) +
                   " (world.gi's probeHdr/probeShadows pin either one "
                                  "independently). High is a re-solve-latency trap in an editor: "
                                  "every geometry or light edit pays for it again.";
        };
        r.cost.clear();
        r.get = [](const iris::ScenePtr &s) { return int(s->giQuality); };
        r.set = [](const iris::ScenePtr &s, int v) { s->giQuality = iris::GiQuality(v); };
        out.push_back(r);
    }
    {
        Row r;
        r.id = "giDdgi";
        r.label = "Photon Irradiance Field (DDGI)";
        r.group = "Global Illumination";
        r.type = RowType::Enum;
        r.tierSpace = TierSpace::Photon;
        r.options = { { "off", "Off", 0 },
                      { "on",  "On",  1 } };
        // Owner option (b), 2026-09-09: every voxel tier feeds the field — it
        // is the one diffuse arm that is right in both open and sealed scenes
        // (rayon2 S1-S3). Low has no voxel volume to feed it from.
        photonColumns(r, 2);
        r.cost = "The irradiance field: a grid of probes over the lit volume storing "
                                "the bounced light arriving from every direction, plus a depth map "
                                "that decides what each probe can actually see. It is the LEAK FIX "
                                "— cone-traced bounce blows out corners because a cone cannot tell "
                                "a wall from empty space. Turning it on turns the voxel-cone "
                                "diffuse OFF (it replaces that term rather than adding to it); "
                                "reflections, probes and planar are untouched. It needs a voxel "
                                "volume to be fed from, and EVERY Photon tier builds one — "
                                "including Low, whose two camera cascades exist for exactly this "
                                "— so every tier turns it on. (It said \"Low cannot\" here for "
                                "months; Low's column has always been 1.)";
        // Where the screen-probe gather runs it is the diffuse and the field its
        // fallback — generated from the engine's gather row, the sentence the
        // World GI panel used to hand-write a second copy of.
        {
            std::vector<std::string> gatherTiers;
            for (PhotonTier t : { PhotonTier::Low, PhotonTier::Medium, PhotonTier::High,
                                  PhotonTier::Epic }) {
                std::string n = photonTierName(t);
                n[0] = char(std::toupper(static_cast<unsigned char>(n[0])));
                gatherTiers.push_back(n);
            }
            if (!gatherTiers.empty()) {
                const std::string last = gatherTiers.back();
                gatherTiers.pop_back();
                const std::string tiers = gatherTiers.empty()
                    ? last
                    : iris::text::join(gatherTiers, ", ") + " and " + last;
                r.cost += iris::text::cat(" At ", tiers, ", wherever rays run, the screen-probe gather is the diffuse and the field is its fallback.");
            }
        }
        // -1 (auto) is what a scene no tier has ever been applied to holds, and
        // the engine resolves it through GiQualityFacts::fieldDefault of the
        // scene's OWN giQuality — so that is what it resolves to here, through
        // the same fact (photonFieldAuto; DDGI-AUTO-1: this read the TIER's
        // column, a second resolution that parted from the engine's the moment a
        // pinned giQuality met a per-quality default). Any tier application
        // writes a concrete 0/1 through.
        r.get = [](const iris::ScenePtr &s) {
            if (s->giDdgi < 0) return photonFieldAuto(s) ? 1 : 0;
            return s->giDdgi > 0 ? 1 : 0;
        };
        r.set = [](const iris::ScenePtr &s, int v) { s->giDdgi = v ? 1 : 0; };
        out.push_back(r);
    }
    // EPIC'S TWO COLUMNS (owner option (b), 2026-09-09): what makes the top
    // tier a tier of its own now that High is DDGI-fed too. Both are ordinary
    // Int rows over document fields the engine already consumes; the engine's
    // quality dial is untouched (it is the RESOLUTION dial, and Epic changes
    // no resolution).
    // THE PROBE CAPTURE SIZE (owner decision 2026-09-13 Q4: "yes halve it but
    // add it to the world settings"). A Photon-tiered row like the four above, so
    // an edit PINS it; every tier's column is 0 = "follow the quality dial",
    // because the halving itself is the engine's default now.
    {
        Row r;
        r.id = "giProbeSize";
        r.label = "Photon Probe Capture Size";
        r.group = "Global Illumination";
        r.type = RowType::Enum;
        r.tierSpace = TierSpace::Photon;
        r.options = { { "auto", "Automatic", 0 },
                      { "128",  "128 px",  128 },
                      { "256",  "256 px",  256 },
                      { "512",  "512 px",  512 } };
        photonColumns(r, 4);
        // GENERATED: "Automatic" is the engine's quality dial and only two
        // tiers build probes at all. The hand-written version claimed 256 "at
        // every tier from Medium up" while High and Epic resolve 512, and
        // Medium builds no probe grid to size (render audit A5).
        r.costAt = [](bool sceneTracesRays) {
            const int high = int(jahshaka::engine::giQualityFacts(
                                     jahshaka::engine::GiQuality::High).probeFaceSize);
            const std::string where =
                tierRaysResolve(PhotonTier::High, sceneTracesRays)
                    ? iris::text::cat(", but here NO shipped tier builds a probe grid: ", tiersWithTechnique(1), " build", (std::count(tiersWithTechnique(1).begin(), tiersWithTechnique(1).end(), ' ') != 0)
                                   ? std::string() : "s", " none, and at ", tiersWithTechnique(2), " the rays are the reflection. This row sizes a grid only where the technique is pinned to ", techniqueLabel(2, false), " or the scene's Ray Tracing row is off.")
                    : iris::text::cat(", and only ", tiersWithTechnique(2), " build a probe grid at all, so ", high, " is the shipped answer wherever this row has anything to size.");
            return iris::text::cat("The pixel size of ONE reflection-probe cube face. A probe is six of them plus a mip chain, so the grid's video memory goes with the SQUARE of this: at 256 a probe is 4.0 MB in HDR and a 32-probe room 128 MB; at 512 it is 16.0 MB and 512 MB. Automatic follows the quality dial (", jahshaka::engine::giQualityFacts(jahshaka::engine::GiQuality::Low)
                                .probeFaceSize, " px at Low, ", jahshaka::engine::giQualityFacts(jahshaka::engine::GiQuality::Medium)
                                .probeFaceSize, " at Medium, ", high, " at High and Epic)") +
                   where +
                   " The roughness blur the renderer convolves into these "
                                  "captures hides the difference on everything but a mirror.";
        };
        r.get = [](const iris::ScenePtr &s) { return iris::bound(0, s->giProbeCaptureSize, 1024); };
        r.set = [](const iris::ScenePtr &s, int v) { s->giProbeCaptureSize = iris::bound(0, v, 1024); };
        out.push_back(r);
    }
    {
        Row r;
        r.id = "giBounces";
        r.label = "Photon Light Bounces";
        r.group = "Global Illumination";
        r.type = RowType::Int;
        r.tierSpace = TierSpace::Photon;
        r.minValue = 0; r.maxValue = 1;
        photonColumns(r, 3);
        // GENERATED: "128^3 at High/Epic" named one resolution for a chain
        // whose four cascades are 128, 128, 64 and 64 (render audit A5).
        r.cost = iris::text::cat("Whether light bounces: 1 (every tier) lets every surface re-emit what it gathers - the lamps', the sky's and every other surface's light - and the renderer converges the whole series in the voxel volume by feedback: a pass a frame over the parts still changing, each stopped once what is left of its series is under 1 %, so an enclosed room and an open scene both reach their limit and there is no count of bounces to pick. The irradiance field is fed from those volumes, so the bounces reach the probe-stored diffuse too. 0 is THE DIRECT STORE ALONE: the voxels hold the lamps' direct light and nothing a surface re-emits (a measurement setting). At ", photonTierVoxelPhrase(PhotonTier::High), " voxels per axis (High and Epic).");
        r.get = [](const iris::ScenePtr &s) { return s->giBounces ? 1 : 0; };
        r.set = [](const iris::ScenePtr &s, int v) { s->giBounces = v != 0; };
        out.push_back(r);
    }
    // ---- Sky ---------------------------------------------------------------
    // THE "Sky Detail" ROW IS GONE (SKY-GPU, 2026-09-13). It was the width the
    // analytic sky was CPU-BAKED at, and there is no bake: the sky is a shader.
    // Nothing replaced it — a tier row has to buy something, and this one now
    // buys nothing.

    // ---- Reflections -------------------------------------------------------
    // This row was declared here with `available = false` before the engine
    // could serve it; the planar lane filled in get/set and flipped the flag,
    // and not one consumer of this file changed. That is what the contract-row
    // idea was for.
    //
    // Epic is 2, not the 4 this row was drafted with. Each active plane is a
    // FULL extra scene render inside the editor's per-frame loop, which shares
    // the machine with Qt, the scripting host and the MCP server: four planes
    // means five scene renders and an editor that stops feeling interactive on
    // any scene heavier than a sample. Two mirrors facing the camera at once is
    // already an unusual composition, so the visible payoff of 3 and 4 is small.
    // The verb and this row still ACCEPT up to 8 for anyone who wants it, and
    // an artist can force which mirror wins; only the preset is conservative.
    {
        Row r;
        r.id = "planarBudget";
        r.label = "Planar Reflections";
        r.group = "Reflections";
        r.type = RowType::Int;
        r.minValue = 0; r.maxValue = 8;
        r.tier[0] = kPlanarTier[0]; r.tier[1] = kPlanarTier[1];
        r.tier[2] = kPlanarTier[2]; r.tier[3] = kPlanarTier[3];
        r.cost = "How many mirror planes may re-render the scene. One extra full "
                                "scene render per plane — the most expensive row in the table. "
                                "CHANGING IT REBUILDS SHADERS: the count is baked into the PBR "
                                "shader, not passed as a uniform, so expect a pause on the next "
                                "frame. Planes are placed per object (Properties > Planar "
                                "Reflector); this is only the cap on how many of them render.";
        r.get = [](const iris::ScenePtr &s) { return planarBudgetOf(s); };
        r.set = [](const iris::ScenePtr &s, int v) { s->planarReflectionBudget = v; };
        out.push_back(r);
    }

    return out;
}

/// The override map is JSON on the document, so values round-trip as doubles.
bool overrideValue(const iris::ScenePtr &scene, const std::string &id, int &out)
{
    if (!scene) return false;
    const iris::Value *v = scene->worldOverrides.find(id);
    if (!v || v->isNull()) return false;
    out = iris::intOr(scene->worldOverrides, id.c_str(), 0);
    return true;
}

}   // namespace

const std::vector<Row> &rows()
{
    static const std::vector<Row> table = buildRows();
    return table;
}

const Row *row(const std::string &id)
{
    for (const Row &r : rows())
        if (r.id == id) return &r;
    return nullptr;
}

std::string rowCost(const Row &r, bool sceneTracesRays)
{
    return r.costAt ? r.costAt(sceneTracesRays) : r.cost;
}

std::string optionLabel(const Row &r, const EnumOption &o, const iris::ScenePtr &scene,
                    bool sceneTracesRays)
{
    return r.optionLabelAt ? r.optionLabelAt(o.value, scene, sceneTracesRays) : o.label;
}

std::vector<ComboItem> comboItems(const Row &r, const iris::ScenePtr &scene, bool sceneTracesRays)
{
    if (r.comboItemsAt) return r.comboItemsAt(scene, sceneTracesRays);
    std::vector<ComboItem> items;
    for (const EnumOption &o : r.options)
        items.push_back({ o.id, optionLabel(r, o, scene, sceneTracesRays), o.value, false, {} });
    return items;
}

bool applyComboItem(const iris::ScenePtr &scene, const Row &r, const ComboItem &item)
{
    if (!scene) return false;
    if (!item.followsTier) return setRowValue(scene, r.id, item.value);
    clearOverride(scene, r.id);
    if (resolved(scene, r) == 0) return setRowValue(scene, r.id, item.value);
    return true;
}

int comboIndexOf(const std::vector<ComboItem> &items, int value)
{
    for (int i = 0; i < items.size(); ++i)
        if (items[i].value == value
            || std::find(items[i].shows.begin(), items[i].shows.end(), value) != items[i].shows.end()) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// THE CONTINUOUS POST-PROCESS PARAMETERS (fix wave 2026-09-07 item 8).
//
// Not tiered, on purpose (see ParamRow's note and world.postFx's): a mode
// switch answers "how much machinery", never "how should this look". What they
// ARE is declared once, here, so the World > Post Process section and the
// world.postFx verb cannot disagree about a range, a label or what a number
// means — which is the exact failure this file was written to prevent for the
// on/off rows and which the parameters had, in miniature, all along (the
// ranges lived inside the verb and nothing else could see them).

static std::vector<ParamRow> buildPostFxParams()
{
    std::vector<ParamRow> out;
    // ---- EXPOSURE (EXPOSURE-1, 2026-09-17) --------------------------------
    //
    // STOPS, all three, on the same axis a camera's block uses. The post chain
    // takes a natural-log `E` and the conversion happens once, at the mirror
    // (iris::lens::toChain) — nothing in the document or in this table holds a
    // chain-unit exposure any more. The `Ev` in the id is what says so, and it
    // is what keeps a script written against the old chain-unit `exposure` from
    // silently regrading a scene: that name is gone and the verb refuses it.
    {
        ParamRow p;
        p.id = "exposureEv";
        p.label = "Exposure";
        p.ownerRowId = "exposureMode";
        p.minValue = -16.0; p.maxValue = 16.0; p.perPixelStep = 0.01; p.decimals = 2;
        p.doc = "The exposure in STOPS: 0 is the grade a new scene's own lights "
                               "derive (a sun and a Sky Light at intensity 1 over the default "
                               "sky put an 18% grey card on the film's grey card), +1 is one "
                               "doubling, -1 one halving. In Manual it IS the exposure; in Auto "
                               "it is the grade the meter works around, and the window below "
                               "says how far either side of it the meter may go. The same unit "
                               "as a camera's own exposure block.";
        p.enabled = [](const iris::ScenePtr &s) { return s->hdrEnabled; };
        p.get = [](const iris::ScenePtr &s) { return double(s->exposure); };
        p.set = [](const iris::ScenePtr &s, double v) { s->exposure = float(v); };
        out.push_back(p);
    }
    {
        ParamRow p;
        p.id = "exposureMin";
        p.label = "Auto Exposure Min";
        p.ownerRowId = "exposureMode";
        p.minValue = -16.0; p.maxValue = 16.0; p.perPixelStep = 0.01; p.decimals = 2;
        p.doc = "The bottom of the window the AUTOMATIC exposure may adapt "
                               "within, in stops AROUND THE EXPOSURE ABOVE: -3.5 lets the meter "
                               "land up to three and a half stops under the exposure you typed, "
                               "and 0 stops means it may not go under it at all. A window of "
                               "0..0 is the exposure you typed and nothing else — the same "
                               "picture Manual renders. Read only in Auto: Manual measures "
                               "nothing, so there is nothing to bound.";
        p.enabled = [](const iris::ScenePtr &s) { return s->hdrEnabled; };
        p.visible = [](const iris::ScenePtr &s) {
            return s->exposureMode == iris::ExposureMode::Auto;
        };
        p.get = [](const iris::ScenePtr &s) { return double(s->exposureMin); };
        p.set = [](const iris::ScenePtr &s, double v) { s->exposureMin = float(v); };
        out.push_back(p);
    }
    {
        ParamRow p;
        p.id = "exposureMax";
        p.label = "Auto Exposure Max";
        p.ownerRowId = "exposureMode";
        p.minValue = -16.0; p.maxValue = 16.0; p.perPixelStep = 0.01; p.decimals = 2;
        p.doc = "The top of the automatic exposure's window, in stops above the "
                               "exposure above. A narrow window is a steadier image; a wide one "
                               "copes with walking from a dark room into daylight. Read only in "
                               "Auto.";
        p.enabled = [](const iris::ScenePtr &s) { return s->hdrEnabled; };
        p.visible = [](const iris::ScenePtr &s) {
            return s->exposureMode == iris::ExposureMode::Auto;
        };
        p.get = [](const iris::ScenePtr &s) { return double(s->exposureMax); };
        p.set = [](const iris::ScenePtr &s, double v) { s->exposureMax = float(v); };
        out.push_back(p);
    }
    {
        ParamRow p;
        p.id = "exposureMeterLow";
        p.label = "Meter Low Percentile";
        p.ownerRowId = "exposureMode";
        p.minValue = 0.0; p.maxValue = 100.0; p.perPixelStep = 0.25; p.decimals = 1;
        p.doc = "WHICH SLICE of what the meter sees it believes, from the dark "
                               "end: 10 throws away the darkest tenth of the metered weight. "
                               "The meter builds a histogram of the frame's log luminance and "
                               "averages between this percentile and the high one, so a deep "
                               "shadow region cannot pull the grade the way it pulls an "
                               "average. 0 keeps everything. Read only in Auto.";
        p.enabled = [](const iris::ScenePtr &s) { return s->hdrEnabled; };
        p.visible = [](const iris::ScenePtr &s) {
            return s->exposureMode == iris::ExposureMode::Auto;
        };
        p.get = [](const iris::ScenePtr &s) { return double(s->exposureMeterLowPercent); };
        p.set = [](const iris::ScenePtr &s, double v) { s->exposureMeterLowPercent = float(v); };
        out.push_back(p);
    }
    {
        ParamRow p;
        p.id = "exposureMeterHigh";
        p.label = "Meter High Percentile";
        p.ownerRowId = "exposureMode";
        p.minValue = 0.0; p.maxValue = 100.0; p.perPixelStep = 0.25; p.decimals = 1;
        p.doc = "The same from the bright end: 90 throws away the brightest "
                               "tenth of the metered weight, which is what keeps a sun disc, "
                               "a blown window or a specular firefly out of the measurement. "
                               "100 keeps everything, and is the closest this meter gets to "
                               "the plain mean it replaced. A pair that keeps nothing is read "
                               "as the whole frame. Read only in Auto.";
        p.enabled = [](const iris::ScenePtr &s) { return s->hdrEnabled; };
        p.visible = [](const iris::ScenePtr &s) {
            return s->exposureMode == iris::ExposureMode::Auto;
        };
        p.get = [](const iris::ScenePtr &s) { return double(s->exposureMeterHighPercent); };
        p.set = [](const iris::ScenePtr &s, double v) { s->exposureMeterHighPercent = float(v); };
        out.push_back(p);
    }
    // THE IMAGE BLOCK (IMAGE-1): the world camera's development, one row per
    // iris::lens::imageParams() entry, under the exposure rows — the world's
    // defaults for the editor's view and every camera (each camera overrides
    // any of them through camera.postFx under the same id).
    for (int i = 0; i < iris::lens::ImageParamCount; ++i) {
        const iris::lens::ImageParamDef &d = iris::lens::imageParams()[i];
        ParamRow p;
        p.id = d.id;
        p.label = d.label;
        p.ownerRowId = "exposureMode";
        p.minValue = d.minValue; p.maxValue = d.maxValue;
        p.perPixelStep = d.perPixelStep; p.decimals = d.decimals;
        p.doc = d.doc ? std::string(d.doc) : std::string();   // fromUtf8(nullptr) was empty
        if (d.advanced)
            p.doc += " (Film, advanced: Unreal's filmic tonemapper.)";
        p.enabled = [](const iris::ScenePtr &s) { return s->hdrEnabled; };
        p.get = [i](const iris::ScenePtr &s) { return double(s->image[i]); };
        p.set = [i](const iris::ScenePtr &s, double v) { s->image[i] = float(v); };
        out.push_back(p);
    }
    {
        // THE OWNER'S R17 ROW, and it sits FIRST of the three bloom parameters
        // because it is the one a person reaches for: "how much bloom", right
        // under the checkbox, where he asked for it. The other two describe
        // WHICH pixels bloom and are the tuning behind it.
        ParamRow p;
        p.id = "bloomAmount";
        p.label = "Bloom Amount";
        p.ownerRowId = "bloom";
        p.minValue = 0.0; p.maxValue = 2.0; p.perPixelStep = 0.01; p.decimals = 2;
        p.doc = "How much of the bloom reaches the picture: 1 is the amount "
                               "this renderer has always drawn, 2 is twice as much and 0 is "
                               "none at all. It multiplies the blurred highlight where the "
                               "grade adds it, so it is linear in the light the bloom "
                               "contributes — and it is the CHEAP dial of the three: the "
                               "threshold and the knee decide which pixels bloom, this one "
                               "only decides how strongly the result is mixed in, and "
                               "changing it rebuilds nothing. 0 renders exactly the picture "
                               "Bloom off renders, with the chain still standing; switching "
                               "Bloom off is what stops paying for it.";
        p.get = [](const iris::ScenePtr &s) { return double(s->bloomAmount); };
        p.set = [](const iris::ScenePtr &s, double v) { s->bloomAmount = float(v); };
        out.push_back(p);
    }
    {
        ParamRow p;
        p.id = "bloomThreshold";
        p.label = "Bloom Threshold";
        p.ownerRowId = "bloom";
        p.minValue = 0.0; p.maxValue = 64.0; p.perPixelStep = 0.05; p.decimals = 2;
        p.doc = "Where the bright pass starts, in the tonemapper's units. High "
                               "values read as highlight bloom; low values as a haze filter "
                               "over the whole image.";
        p.get = [](const iris::ScenePtr &s) { return double(s->bloomThreshold); };
        p.set = [](const iris::ScenePtr &s, double v) { s->bloomThreshold = float(v); };
        out.push_back(p);
    }
    {
        // ADDENDUM A-6. The renderer's bright pass has always taken TWO
        // thresholds (min, full) and the engine hard-coded full = min + 2.
        // Exposed as a WIDTH rather than a second absolute value: a width
        // cannot invert, so the renderer's own `full <= min` clamp becomes
        // unreachable instead of being a state the panel can ask for and the
        // renderer silently refuses. Default 2.0 = byte-identical to before.
        ParamRow p;
        p.id = "bloomKnee";
        p.label = "Bloom Knee";
        p.ownerRowId = "bloom";
        p.minValue = 0.01; p.maxValue = 32.0; p.perPixelStep = 0.05; p.decimals = 2;
        p.doc = "How wide the ramp above the threshold is. A narrow knee is a "
                               "hard cut — only the brightest pixels bloom; a wide one fades "
                               "the effect in across a range of brightnesses.";
        p.get = [](const iris::ScenePtr &s) { return double(s->bloomKnee); };
        p.set = [](const iris::ScenePtr &s, double v) { s->bloomKnee = float(v); };
        out.push_back(p);
    }
    {
        ParamRow p;
        p.id = "ssaoPower";
        p.label = "AO Power";
        p.ownerRowId = "ssao";
        p.minValue = 0.1; p.maxValue = 8.0; p.perPixelStep = 0.01; p.decimals = 2;
        p.doc = "Contrast of the occlusion term — how dark a crease gets.";
        p.get = [](const iris::ScenePtr &s) { return double(s->ssaoPower); };
        p.set = [](const iris::ScenePtr &s, double v) { s->ssaoPower = float(v); };
        out.push_back(p);
    }
    {
        ParamRow p;
        p.id = "ssaoRadius";
        p.label = "AO Radius";
        p.ownerRowId = "ssao";
        p.minValue = 0.05; p.maxValue = 64.0; p.perPixelStep = 0.02; p.decimals = 2;
        p.doc = "How far the occlusion looks, in metres, and it is a CONTACT "
                               "scale by default (0.35 m) rather than a room scale. Photon "
                               "already computes the occlusion of the ambient at every scale "
                               "its voxels resolve, from the real geometry; what is left for a "
                               "screen-space march is what falls between the voxels, which is "
                               "centimetres. Raising it does not add contact shadowing, it adds "
                               "a second and cruder copy of the bounce's own occlusion \u2014 "
                               "measured on a 4 m cube, a 2 m radius darkened the bottom 1.55 m "
                               "of the wall, sunlight included. Too small and only the tightest "
                               "corners darken.";
        p.get = [](const iris::ScenePtr &s) { return double(s->ssaoRadius); };
        p.set = [](const iris::ScenePtr &s, double v) { s->ssaoRadius = float(v); };
        out.push_back(p);
    }
    {
        ParamRow p;
        p.id = "distortionStrength";
        p.label = "Distortion Strength";
        p.ownerRowId = "distortion";
        p.minValue = 0.0; p.maxValue = 8.0; p.perPixelStep = 0.01; p.decimals = 2;
        p.doc = "A global multiplier on every distortion material's own "
                               "strength — one dial for how much the whole scene shimmers. "
                               "0 is inert: the frame is bit for bit the frame with no "
                               "distortion at all.";
        p.get = [](const iris::ScenePtr &s) { return double(s->distortionStrength); };
        p.set = [](const iris::ScenePtr &s, double v) { s->distortionStrength = float(v); };
        out.push_back(p);
    }
    return out;
}

const std::vector<ParamRow> &postFxParams()
{
    static const std::vector<ParamRow> table = buildPostFxParams();
    return table;
}

const ParamRow *postFxParam(const std::string &id)
{
    for (const ParamRow &p : postFxParams())
        if (p.id == id) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// THE FOG'S ROWS (FOG-ATMO-1). The ranges are the panel's slider ranges, which
// they always were; world.fog writes a script's number as given (the engine
// clamps the densities at zero), so a range here never refuses a script.
static std::vector<ParamRow> buildFogParams()
{
    std::vector<ParamRow> out;
    auto add = [&out](const char *id, const char *label, double lo, double hi, int decimals,
                      const std::string &doc, float iris::Scene::*field) {
        ParamRow p;
        p.id = id;
        p.label = label;
        p.ownerRowId = "fog";
        p.minValue = lo; p.maxValue = hi; p.decimals = decimals;
        p.perPixelStep = (hi - lo) / 500.0;
        p.doc = doc;
        p.enabled = [](const iris::ScenePtr &s) { return s->fogEnabled; };
        p.get = [field](const iris::ScenePtr &s) { return double((*s).*field); };
        p.set = [field](const iris::ScenePtr &s, double v) { (*s).*field = float(v); };
        out.push_back(p);
    };
    // Density is small by nature (the default scene's 0.0071), so the rows
    // carry real decimals — a two-decimal row would be a two-step control.
    add("density", "Fog Density", 0.0, 0.5, 4,
        "How much of a surface's colour is lost per world unit, on top of the air "
                       "itself: a surface 1/density units away keeps half its colour, and by "
                       "4.32/density it has all but disappeared. 0.01 = half gone at 100 units. "
                       "Under the Realistic sky the air already hazes the distance on its own "
                       "(its turbidity, the sky's Sun Haze), and this adds fog to it.",
        &iris::Scene::fogDensity);
    add("heightDensity", "Height Fog Density", 0.0, 0.5, 4,
        "A second layer of fog whose density falls off with height — ground mist "
                       "and valley fog — in the same colour as the distance fog. 0 turns it off "
                       "entirely.",
        &iris::Scene::fogHeightDensity);
    add("heightFalloff", "Height Falloff", 0.0, 2.0, 3,
        "How fast the height layer thins out as you rise. Larger = a shallower, "
                       "sharper-edged layer; the density halves every 1/falloff units above the "
                       "level below.",
        &iris::Scene::fogHeightFalloff);
    add("heightLevel", "Height Level", -100.0, 100.0, 2,
        "The world height at which the height layer has its full density — the "
                       "surface of the mist, usually the ground.",
        &iris::Scene::fogHeightLevel);
    return out;
}

const std::vector<ParamRow> &fogParams()
{
    static const std::vector<ParamRow> table = buildFogParams();
    return table;
}

const ParamRow *fogParam(const std::string &id)
{
    for (const ParamRow &p : fogParams())
        if (p.id == id) return &p;
    return nullptr;
}

std::string fogParamSceneKey(const ParamRow &p)
{
    return "fog" + iris::text::asciiUpper(p.id.substr(0, 1)) + p.id.substr(1);
}

bool fogColourAuthored(const iris::ScenePtr &scene)
{
    return !scene || scene->skyType != iris::SkyType::REALISTIC;
}

const std::vector<std::string> &postFxRowIds()
{
    // ORDER IS THE PANEL'S ORDER, and it is the frame's order: the scene target
    // and its grade first, then what rides on it, then what runs after the
    // tonemap, then the two that are neither (reflections, refraction).
    static const std::vector<std::string> ids = {
        "hdr", "exposureMode",
        "exposureMetering", "bloom",
        "ssao",
        "smaa", "ssr",
        "reflectionResolution", "refractions",
        "distortion",
    };
    return ids;
}

// ---------------------------------------------------------------------------
// PHOTON (GI_UNIFIED_SPEC.md §2 / P2) — the unified realtime-GI dial.
//
// Everything below manipulates the three backing fields DIRECTLY rather than
// going through setRowValue(), for two reasons: applying a tier must not record
// pins (setMode's rule, and the bug the panel's `*` marker made visible), and
// the `photon` row's own set() runs from inside the registry — reaching back
// into rows() from there is a re-entrancy nobody should have to reason about.
// The tier table itself lives ABOVE buildRows (the rows derive their columns
// from it); what follows is the bookkeeping that reads it.

namespace {

int tierIndex(PhotonTier t) { return iris::bound(0, int(t), 3); }

bool pinned(const iris::ScenePtr &s, const char *id)
{
    return s && s->worldOverrides.find(id) != nullptr;
}

}   // namespace

std::string photonRowId() { return "photon"; }

std::vector<std::string> photonRowIds()
{
    return { "giMode", "giQuality",
             "giDdgi", "giBounces",
             "giProbeSize" };
}

std::string photonTierName(PhotonTier t)
{
    static const char *names[4] = { "low", "medium", "high", "epic" };
    return names[tierIndex(t)];
}

PhotonTier photonTierFromName(const std::string &name, bool *ok)
{
    const std::string n = iris::text::asciiLower(std::string(iris::text::trim(name)));
    if (ok) *ok = true;
    if (n == "low")    return PhotonTier::Low;
    if (n == "medium") return PhotonTier::Medium;
    if (n == "high")   return PhotonTier::High;
    if (n == "epic")   return PhotonTier::Epic;
    if (ok) *ok = false;
    return PhotonTier::Epic;
}

std::vector<std::string> photonTierNames()
{
    return { "low", "medium",
             "high", "epic" };
}

PhotonTier photonTier(const iris::ScenePtr &scene)
{
    if (!scene) return PhotonTier::Epic;
    return PhotonTier(iris::bound(0, scene->giTier, 3));
}

bool photonEnabled(const iris::ScenePtr &scene)
{
    return scene && scene->giMode != iris::GiMode::OFF;
}

int photonTechnique(PhotonTier t) { return kPhotonTable[tierIndex(t)].technique; }
int photonQuality(PhotonTier t)   { return kPhotonTable[tierIndex(t)].quality; }
int photonDdgi(PhotonTier t)      { return photonFieldColumn(kPhotonTable[tierIndex(t)]); }
bool photonFieldAuto(const iris::ScenePtr &scene)
{
    // OgreScene::ddgiWanted's own two terms: a voxel technique to feed it, and the
    // quality's fieldDefault.
    if (!scene || scene->giMode == iris::GiMode::OFF) return false;
    return jahshaka::engine::giQualityFacts(
               jahshaka::engine::GiQuality(iris::bound(0, int(scene->giQuality), 3))).fieldDefault;
}
int photonBounces(PhotonTier t)   { return kPhotonTable[tierIndex(t)].bounces; }
int photonProbeSize(PhotonTier t) { return kPhotonTable[tierIndex(t)].probeSize; }

// THE GATHER COLUMN IS A PROJECTION, NOT A COPY (PHOTON-GATHER-1d): the engine's
// tier table holds the gather row (GiParams.h GiGatherFacts — on/off, stride,
// octahedral resolution, adaptive cap) and this reads it for the tier's quality
// row, so the column cannot drift from what the renderer does.
jahshaka::engine::GiGatherFacts photonGather(PhotonTier t)
{
    return jahshaka::engine::giQualityFacts(
               jahshaka::engine::GiQuality(iris::bound(0, photonQuality(t), 3)),
               jahshaka::engine::GiViewProfile::Desktop)
        .gather;
}

jahshaka::engine::GiGatherFacts photonVrGather(PhotonTier t)
{
    return jahshaka::engine::giQualityFacts(
               jahshaka::engine::GiQuality(iris::bound(0, photonQuality(t), 3)),
               jahshaka::engine::GiViewProfile::Vr)
        .gather;
}

// ---------------------------------------------------------------------------
// WHAT A TIER IS, IN WORDS, GENERATED (render audit A5).
//
// Five tooltips in this file used to describe a renderer that did not exist —
// "Medium voxelizes at twice the resolution" (both are 64), "32/64/128 voxels
// per axis" (the cascade chain, on at every tier, is 64/64/128), "256 at every
// tier from Medium up" (High and Epic are 512), "128^3 at High/Epic" (two of
// the four cascades are 64) and "Low cannot [feed the field]" (Low's column has
// always been 1). Every one of them was a HAND COPY of a number that lives
// somewhere else. These functions read the two tables instead: kPhotonTable
// above, and the engine's `giQualityFacts`.
namespace {

std::string metres(float v)
{
    // Two decimals, trailing zeros trimmed: "5", "0.16", "1.88".
    std::string s = iris::text::fixed(double(v), 2);   // QString::number(v, 'f', 2)'s digits
    while (s.find('.') != std::string::npos && (iris::text::endsWith(s, "0") || iris::text::endsWith(s, ".")))
        s.pop_back();
    return s;
}

/// The engine's physical facts for a tier's quality column.
jahshaka::engine::GiQualityFacts factsFor(PhotonTier t)
{
    return jahshaka::engine::giQualityFacts(
        jahshaka::engine::GiQuality(iris::bound(0, photonQuality(t), 3)));
}

}   // namespace

std::string photonTierVoxelPhrase(PhotonTier t)
{
    const auto facts = factsFor(t);
    std::vector<int> seen;
    for (int i = 0; i < facts.cascadeCount; ++i)
        if (std::find(seen.begin(), seen.end(), facts.cascades[i].resolution) == seen.end())
            seen.push_back(facts.cascades[i].resolution);
    std::sort(seen.begin(), seen.end());
    std::vector<std::string> parts;
    for (int r : seen) parts.push_back(std::to_string(r));
    if (parts.size() == 1) return parts.front();
    const std::string last = parts.back();
    parts.pop_back();
    return iris::text::join(parts, ", ") + " and " + last;
}

int photonTierProbeFaceSize(PhotonTier t)
{
    const int pinned = photonProbeSize(t);
    return pinned > 0 ? pinned : int(factsFor(t).probeFaceSize);
}

namespace {
/// THE ONE PREDICATE (STUDIO-CRUD-1 fix round): a quality column whose
/// reflections the engine traces, met with a scene that traces on this
/// machine. probeGridByRays asks it of a scene, tierRaysResolve of a tier.
bool raysAreTheReflection(int giQuality, bool sceneTracesRays)
{
    return sceneTracesRays
           && jahshaka::engine::giQualityFacts(
                  jahshaka::engine::GiQuality(iris::bound(0, giQuality, 3)))
                  .rayReflections;
}
}   // namespace

bool probeGridByRays(const iris::ScenePtr &scene, bool sceneTracesRays)
{
    return scene && raysAreTheReflection(int(scene->giQuality), sceneTracesRays);
}

bool reflectionsTraced(const iris::ScenePtr &scene, bool sceneTracesRays)
{
    return scene && scene->giMode != iris::GiMode::OFF &&
           raysAreTheReflection(int(scene->giQuality), sceneTracesRays);
}

bool tierRaysResolve(PhotonTier t, bool sceneTracesRays)
{
    return raysAreTheReflection(photonQuality(t), sceneTracesRays);
}

std::string techniqueLabel(int technique, bool raysResolve)
{
    switch (technique) {
    case 0:  return "Off";
    case 1:  return "VCT";
    default: return raysResolve ? "VCT + rays" : "VCT + probes";
    }
}

std::string probeGridByRaysReason()
{
    return "the rays are the reflection here — at High and Epic, wherever this scene traces on this "
    "machine, no reflection-probe grid is built: the screen march, the traced rays (a hit lit "
    "from its surface card, the decode or the voxels) and the voxel cone with the sky as its "
    "escape are the reflection, and a planar mirror stays a planar mirror. Turn the scene's "
    "Ray Tracing row off (world.rayTracing(\"off\")) or pick Low or Medium for a probe grid.";
}

std::string photonTierSentence(PhotonTier t)
{
    const auto facts = factsFor(t);
    std::string out = photonTierName(t);
    out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    out += ": ";

    {
        std::vector<std::string> rows;
        for (int i = 0; i < facts.cascadeCount; ++i) {
            const auto &c = facts.cascades[i];
            rows.push_back(iris::text::cat(metres(c.halfSize), " m at ", c.resolution, " cubed (", metres(jahshaka::engine::giCascadeCell(c)), " m cells)"));
        }
        out += iris::text::cat(facts.cascadeCount, " camera-centred voxel cascade", facts.cascadeCount == 1 ? std::string() : "s", " — ", iris::text::join(rows, ", "));
    }

    // THE DIFFUSE, from the gather row (PHOTON-GATHER-1d): where rays run, the
    // screen-probe gather IS the diffuse and the field its fallback; below it the
    // field (and the cones) are the diffuse.
    const jahshaka::engine::GiGatherFacts gather = photonGather(t);
    out += iris::text::cat("; the diffuse from the screen-probe gather where rays run (", gather.octRes * gather.octRes, " rays a probe, a probe per ", gather.stride, "x", gather.stride, " px)");
    out += photonDdgi(t) ? ", the irradiance field its fallback"
                         : ", no irradiance field";
    // ...AND WHERE RAYS DO NOT RUN (PHOTON-7 stage 7, 7-6): the screen integrate writes the same products from the world
    // caches - the field's diffuse (the cones' beyond its window), the cone's specular, the area lights' visibility
    // through the voxel chain's shadow march
    out += photonDdgi(t) ? " (without rays, the field's diffuse and the voxel cones' beyond it, from the "
                                          "screen integrate)"
                         : " (without rays, the voxel cones' diffuse, from the screen integrate)";
    // ...AND IN A HEADSET (PHOTON-GA-VR): the tier table's VR column, read,
    // never assumed — the same gather, or its own density.
    const jahshaka::engine::GiGatherFacts vr = photonVrGather(t);
    if (vr.stride != gather.stride || vr.octRes != gather.octRes)
        out += iris::text::cat(" (in a headset a probe per ", vr.stride, "x", vr.stride, " px, ", vr.octRes * vr.octRes, " rays)");
    else
        out += " (in a headset too, per eye)";
    out += photonBounces(t) > 0 ? "; the light bounces, converged" : "; no bounce";

    // The probe grid is the TECHNIQUE column, not the quality one: only the
    // hybrid (ordinal 2) builds one — and at a RAY tier only where the scene
    // does not trace (PHOTON-F12-PCC: the rays are the reflection there).
    if (photonTechnique(t) == 2) {
        out += facts.rayReflections
                   ? iris::text::cat("; where rays run the reflections are traced and no reflection-probe grid is built, "
                                     "elsewhere a grid at ", photonTierProbeFaceSize(t), " px per cube face")
                   : iris::text::cat("; a reflection-probe grid at ", photonTierProbeFaceSize(t), " px per cube face");
        if (facts.probeHdrDefault) out += ", HDR";
        if (facts.probeShadowsDefault) out += ", shadowed";
    } else {
        out += "; no reflection probes";
    }
    return out + ".";
}

const std::vector<OffscreenPicture> &offscreenPictures()
{
    // THE ROWS, and where each is carried out (the grade doors are
    // IEditorViewport::ScreenshotGrade; the view shape is the engine's
    // PostFxDesc::allowOffscreen — an offscreen view carries no chain unless it
    // opts in, which is what makes a lower picture a DECLARED one).
    static const std::vector<OffscreenPicture> kinds = {
        { "viewport", true, std::string() },
        // The user's Screenshot, MCP's postFx shot, camera.screenshot({postFx}):
        // the whole chain, so every chain-borne Photon term runs.
        { "screenshot.scene", true, std::string() },
        { "screenshot.viewport", true, std::string() },
        // THE TEST PICTURE: no post chain. Its GI is the tier's — the voxel chain, the
        // field, and the screen-probe gather (every view of a ray scene gathers) — but
        // none of the chain-borne terms.
        { "screenshot.plain", false,
          "the tier's GI (the voxel chain, the irradiance field and the "
                         "screen-probe gather) with NO post chain: no screen "
                         "march, no traced reflections, no sun contact, no SSAO, no grade — the "
                         "exactly reproducible instrument every pixel suite asserts" },
        { "screenshot.tonemap", false,
          "the plain picture plus the deterministic filmic grade at the scene's "
                         "exposure (the thumbnail grade)" },
        // Project preview tiles and the asset viewer take the thumbnail grade: cheap
        // on purpose, and declared so.
        { "projectTile", false,
          "the tonemap grade (screenshot.tonemap) at tile size" },
        // A reflection probe is a photograph for the SPECULAR term; a chain inside it
        // would count the screen-space and traced terms twice.
        { "probeCapture", false,
          "the probe workspace's six faces: the scene lit directly and by the "
                         "voxel cascades, no post chain, HDR and shadowed per the tier" },
        // A surface-cache card is the hit's lighting, lit on the GPU from its own
        // capture (JahCardLight).
        { "cardCapture", false,
          "the card's albedo, normal and emissive, lit on the GPU: direct + "
                         "the environment half + emissive, and an indirect half marched "
                         "through the voxels on its own budget" },
    };
    return kinds;
}

std::string photonTierSummary()
{
    std::vector<std::string> lines;
    for (const std::string &name : photonTierNames()) {
        bool ok = false;
        const PhotonTier t = photonTierFromName(name, &ok);
        if (ok) lines.push_back(photonTierSentence(t));
    }
    return iris::text::join(lines, " ");
}

namespace {
/// The four values a scene RENDERS, in kPhotonTable column order.
void photonHave(const iris::ScenePtr &s, int have[kPhotonRowCount])
{
    have[0] = int(s->giMode);
    have[1] = int(s->giQuality);
    have[2] = s->giDdgi < 0 ? (photonFieldAuto(s) ? 1 : 0) : (s->giDdgi > 0 ? 1 : 0);
    have[3] = s->giBounces ? 1 : 0;
    have[4] = iris::bound(0, s->giProbeCaptureSize, 1024);
}
}   // namespace

void setPhoton(const iris::ScenePtr &scene, bool enabled, PhotonTier tier)
{
    if (!scene) return;
    const PhotonRow &row = kPhotonTable[tierIndex(tier)];
    scene->giTier = tierIndex(tier);

    if (!enabled) {
        // OFF writes exactly ONE field. The technique field IS the on/off
        // switch (there is no second flag to disagree with the renderer), so a
        // pinned technique cannot survive an off — "off, but pinned to VCT" is
        // a state nothing can render, and the pin would silently un-switch
        // Photon at the next tier application. The machinery rows are left
        // alone: writing a tier's quality into a scene that renders no GI would
        // be churn in the document and in every diff of it, and re-enabling
        // applies the tier anyway.
        scene->worldOverrides.erase("giMode");
        scene->giMode = iris::GiMode::OFF;
        return;
    }
    // The machinery: written unless the user pinned it.
    if (!pinned(scene, "giQuality")) scene->giQuality = iris::GiQuality(iris::bound(0, row.quality, 3));
    // THE FIELD IS AUTO UNDER A TIER (DDGI-AUTO-1): -1, which the engine resolves
    // through GiQualityFacts::fieldDefault of the quality it runs at — the one
    // resolution; a tier writes no concrete copy of its column for it to part from.
    if (!pinned(scene, "giDdgi"))    scene->giDdgi = -1;
    if (!pinned(scene, "giBounces")) scene->giBounces = row.bounces != 0;
    if (!pinned(scene, "giProbeSize")) scene->giProbeCaptureSize = iris::bound(0, row.probeSize, 1024);
    if (!pinned(scene, "giMode")) scene->giMode = iris::GiMode(iris::bound(1, row.technique, 2));
    else if (scene->giMode == iris::GiMode::OFF) {
        // A pin of "off" is what an Advanced technique picker set to Off would
        // record; turning Photon on means the tier's technique, so the pin goes.
        scene->worldOverrides.erase("giMode");
        scene->giMode = iris::GiMode(iris::bound(1, row.technique, 2));
    }
}

bool photonCustom(const iris::ScenePtr &scene)
{
    return !photonDeviations(scene).empty();
}

std::vector<std::string> photonDeviations(const iris::ScenePtr &scene)
{
    std::vector<std::string> out;
    // With Photon off there is nothing to deviate FROM: the picture is "no GI"
    // whatever the machinery rows say, so the tier row reads Off, not Custom.
    if (!scene || !photonEnabled(scene)) return out;
    const PhotonRow &want = kPhotonTable[tierIndex(photonTier(scene))];
    int have[kPhotonRowCount];
    photonHave(scene, have);
    const std::vector<std::string> ids = photonRowIds();
    for (int i = 0; i < kPhotonRowCount; ++i) {
        if (have[i] == photonColumn(want, i)) continue;
        const Row *r = row(ids[i]);
        out.push_back((r ? r->label : ids[i]));
    }
    return out;
}

void clearPhotonOverrides(const iris::ScenePtr &scene)
{
    if (!scene) return;
    for (const std::string &id : photonRowIds()) scene->worldOverrides.erase(id);
    setPhoton(scene, photonEnabled(scene), photonTier(scene));
}

std::string modeName(Mode m)
{
    switch (m) {
    case Mode::Low:    return "low";
    case Mode::Medium: return "medium";
    case Mode::High:   return "high";
    case Mode::Epic:   return "epic";
    case Mode::Custom: break;
    }
    return "custom";
}

Mode modeFromName(const std::string &name, bool *ok)
{
    const std::string n = iris::text::asciiLower(std::string(iris::text::trim(name)));
    if (ok) *ok = true;
    if (n == "low")    return Mode::Low;
    if (n == "medium") return Mode::Medium;
    if (n == "high")   return Mode::High;
    if (n == "epic")   return Mode::Epic;
    if (n == "custom") return Mode::Custom;
    if (ok) *ok = false;
    return Mode::Custom;
}

std::vector<std::string> modeNames()
{
    return { "low", "medium",
             "high", "epic" };
}

Mode pickedMode(const iris::ScenePtr &scene)
{
    if (!scene) return Mode::Custom;
    const int m = scene->worldMode;
    return (m >= 0 && m <= 3) ? Mode(m) : Mode::Custom;
}

Mode mode(const iris::ScenePtr &scene)
{
    // THE HONEST ANSWER (WORLD-MODE-1, owner 2026-10-09): the picked mode, unless Photon
    // has left that mode's column (its own dropdown moved it, or it was switched off) —
    // the document keeps the pick; the Custom is computed, never stored.
    const Mode m = pickedMode(scene);
    if (m == Mode::Custom) return Mode::Custom;
    const Row *photon = row(photonRowId());
    if (photon && resolved(scene, *photon) != photon->tier[int(m)]) return Mode::Custom;
    return m;
}

int tierValue(const Row &r, Mode m, const iris::ScenePtr &scene)
{
    // A Photon-tiered row answers in the PHOTON tier space: its columns are the
    // GI dial's tiers, and the World Mode has nothing to say about it directly
    // (it drives the `photon` row, which drives this one).
    if (r.tierSpace == TierSpace::Photon) return r.tier[int(photonTier(scene))];
    // NO TIER RESOLVES THIS ROW (TierSpace::None): its value is the backing field.
    if (r.tierSpace == TierSpace::None) return resolved(scene, r);
    if (m == Mode::Custom) return resolved(scene, r);
    return r.tier[int(m)];
}

int resolved(const iris::ScenePtr &scene, const Row &r)
{
    // The write-through invariant: the backing field IS the resolved value.
    if (r.get && scene) return r.get(scene);
    // A row with no backing field yet (a declared contract, §9.2) has nowhere to
    // write through TO, so it resolves the long way: an explicit pin, else the
    // current tier's value, else Epic's as the documented shape of the row.
    int v = 0;
    if (overrideValue(scene, r.id, v)) return v;
    const Mode m = pickedMode(scene);
    return m == Mode::Custom ? r.tier[3] : r.tier[int(m)];
}

std::string source(const iris::ScenePtr &scene, const Row &r)
{
    if (!scene) return "custom";
    if (scene->worldOverrides.find(r.id)) return "override";
    // A row NO TIER RESOLVES was never set by a mode, so saying "mode" would be
    // a claim about a dial that does not own it (EXPOSURE-1).
    if (r.tierSpace == TierSpace::None) return "custom";
    // "mode" only while the row HOLDS its column (WORLD-MODE-1): a row the Photon dropdown,
    // a Photon switch or a sibling setter moved off the picked mode's value is custom.
    // A PHOTON-TIERED row (the machinery under Photon) answers exactly as photonDeviations
    // does — one rule for the `*` marker and the Photon section's Custom: with Photon OFF
    // there is nothing to deviate from (the off picture is the mode's own state for these
    // rows; the `photon` row itself carries the Custom), and with Photon on a row is custom
    // iff its rendered value leaves the Photon tier's column.
    if (r.tierSpace == TierSpace::Photon) {
        const auto &photonIds = photonRowIds();
        if (std::find(photonIds.begin(), photonIds.end(), r.id) != photonIds.end()) {
            const std::vector<std::string> deviations = photonDeviations(scene);
            return std::find(deviations.begin(), deviations.end(), r.label) != deviations.end() ? "custom" : "mode";
        }
    }
    const Mode m = pickedMode(scene);
    if (m == Mode::Custom) return "custom";
    return resolved(scene, r) == tierValue(r, m, scene) ? "mode"
                                                        : "custom";
}

namespace {

/// Refuses a value the row cannot hold: an enum row takes only its listed
/// values, an int row only its range, a bool row only 0/1.
bool validate(const Row &r, int value)
{
    switch (r.type) {
    case RowType::Bool: return value == 0 || value == 1;
    case RowType::Int:  return value >= r.minValue && value <= r.maxValue;
    case RowType::Enum:
        for (const EnumOption &o : r.options)
            if (o.value == value) return true;
        return false;
    }
    return false;
}

/// Write-through: the tier value lands in the backing field. A row with no
/// backing field is SKIPPED, deliberately — inserting its tier value into
/// worldOverrides would mark it "pinned by the user" for ever after, and it
/// would then survive every future mode switch (the bug the panel's `*` marker
/// made visible the first time a mode was applied).
void writeField(const iris::ScenePtr &scene, const Row &r, int value)
{
    if (r.set) r.set(scene, value);
}

}   // namespace

void setMode(const iris::ScenePtr &scene, Mode m)
{
    if (!scene) return;
    scene->worldMode = int(m);
    if (m == Mode::Custom) return;   // no tier to write through
    // THE IDENTITY COVERS THE TECHNIQUE (WORLD-MODE-1 fix round, owner: "switching World Mode
    // back resets Photon to what matches it"): a Photon Technique pinned under Advanced
    // (giMode, e.g. VCT under Epic) would survive the `photon` row's setPhoton below, which
    // honours a non-off giMode pin — the selector would read Epic while Photon reads Custom.
    // A World Mode pick drops that pin, so the row writes the mode's own technique; a Photon
    // edit after the pick makes the mode Custom again as before.
    scene->worldOverrides.erase("giMode");
    for (const Row &r : rows()) {
        // A Photon-tiered row is written by the `photon` row (which IS in this
        // loop and honours the same pins) — never twice, and never from the
        // world tier's columns, which are not this row's tier space at all.
        if (r.tierSpace != TierSpace::World) continue;
        if (scene->worldOverrides.find(r.id)) continue;   // pinned: survives the switch
        writeField(scene, r, r.tier[int(m)]);
    }
}

bool setRowValue(const iris::ScenePtr &scene, const std::string &id, int value, bool recordOverride)
{
    if (!scene) return false;
    const Row *r = row(id);
    if (!r || !validate(*r, value)) return false;
    writeField(scene, *r, value);
    // THE PHOTON ROW IS NEVER PINNED (WORLD-MODE-1): re-picking a World Mode must snap Photon
    // back to the mode's column, so a set of it is an edit, never a pin.
    if (id == photonRowId()) return true;
    // A row with no backing field can ONLY be remembered as a pin, so an
    // explicit set of one always records (it is a deliberate user choice —
    // unlike setMode's sweep, which never touches those rows).
    // (worldOverrides is always an Object - the scene's own, the reader's object, the reset - so set cannot refuse:
    // a false would be a scene whose override map is not one.)
    if ((recordOverride || !r->set) && !scene->worldOverrides.set(id, iris::Value(value))) return false;
    return true;
}

void pinRowValue(const iris::ScenePtr &scene, const std::string &id, int value)
{
    if (!scene || !row(id)) return;
    if (id == photonRowId()) return;   // never pinned (WORLD-MODE-1, setRowValue)
    (void)scene->worldOverrides.set(id, iris::Value(value));   // always an Object (setRowValue's note)
}

bool clearOverride(const iris::ScenePtr &scene, const std::string &id)
{
    if (!scene || !scene->worldOverrides.erase(id)) return false;
    const Row *r = row(id);
    const Mode m = pickedMode(scene);
    // A Photon-tiered row falls back through the PHOTON tier, and it does so via
    // setPhoton rather than a bare field write: the technique row doubles as the
    // on/off switch, so writing its tier value directly would switch GI back on
    // for a scene that has it off.
    if (r && r->tierSpace == TierSpace::Photon) {
        setPhoton(scene, photonEnabled(scene), photonTier(scene));
        return true;
    }
    // A ROW NO TIER RESOLVES HAS NOTHING TO FALL BACK TO, and `r->tier[]` is a
    // block of zeros nobody ever filled in — writing it would silently reset
    // the field to whatever zero happens to mean (for Exposure Mode: MANUAL, so
    // an Auto scene would flip the moment ANY pin anywhere was cleared, since
    // clearOverrides walks the whole map). Dropping the pin is the whole
    // operation for these rows.
    if (r && r->tierSpace == TierSpace::None) return true;
    // Fall back to the tier value. In Custom mode there is no tier, so the
    // field simply keeps whatever it had — dropping the pin is all that happens.
    if (r && m != Mode::Custom) writeField(scene, *r, r->tier[int(m)]);
    return true;
}

void clearOverrides(const iris::ScenePtr &scene)
{
    if (!scene) return;
    // In the order QJsonObject's keys() gave (sorted: the ids are ASCII), so the clears run as they always ran.
    std::vector<std::string> ids;
    if (scene->worldOverrides.kind() == iris::Kind::Object)
        for (const iris::Value::Member &m : scene->worldOverrides.asObject()) ids.push_back(m.first);
    std::sort(ids.begin(), ids.end());   // QStringList::sort: case-sensitive (the ids are ASCII: the same order)
    for (const std::string &id : ids) clearOverride(scene, id);
}

std::string valueLabel(const Row &r, int value)
{
    if (r.type == RowType::Bool) return value ? "On" : "Off";
    for (const EnumOption &o : r.options)
        if (o.value == value) return o.label;
    return std::to_string(value);
}

std::string valueId(const Row &r, int value)
{
    if (r.type == RowType::Bool) return value ? "on" : "off";
    for (const EnumOption &o : r.options)
        if (o.value == value) return o.id;
    return std::to_string(value);
}

bool valueFromId(const Row &r, const std::string &id, int &out)
{
    const std::string n = iris::text::asciiLower(std::string(iris::text::trim(id)));
    if (r.type == RowType::Bool) {
        if (n == "on"  || n == "true"  || n == "1")
            { out = 1; return true; }
        if (n == "off" || n == "false" || n == "0")
            { out = 0; return true; }
        return false;
    }
    // CASE-INSENSITIVE, and the caller's spelling is what is being forgiven —
    // not the table's. Every option id was lowercase until EXPOSURE-2 gave the
    // metering row `centreWeighted`, which is the DOCUMENT's own serialised
    // spelling (iris::exposureMeteringName) and must stay that on both surfaces;
    // comparing against a lowercased `n` silently refused it. Every existing id
    // is unaffected, being lowercase already.
    for (const EnumOption &o : r.options)
        if (iris::text::iequalsAscii(o.id, n)) { out = o.value; return true; }
    std::string_view number(n);   // QString::toInt's answer: a leading '+' taken, an int's range
    if (!number.empty() && number.front() == '+') number.remove_prefix(1);
    const std::optional<std::int64_t> v = iris::text::parseInt(number);
    if (!v || *v < std::numeric_limits<int>::min() || *v > std::numeric_limits<int>::max()) return false;
    out = int(*v);
    return true;
}

}   // namespace worldmodes
