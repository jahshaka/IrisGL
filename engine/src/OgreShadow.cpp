// THE SHADOW ATLAS: how many maps exist, where they sit in one texture, and
// which of them are static (SPECS/SHADOW_TOOLING_SPEC.md).
//
// WHY THIS TU EXISTS AT ALL, i.e. why we no longer call
// `ShadowNodeHelper::createShadowNodeWithSettings`:
//
//  1. THE DEFECT. The helper's ShadowParamVec fixed the number of focused
//     (point/spot) maps at TWO, forever. Ogre gives each non-PSSM param exactly
//     one map for one light and, at frame time, fills the slots with the
//     closest casters and silently drops the rest
//     (OgreCompositorShadowNode.cpp:403-430; ":the Nth closest lights don't
//     cast shadows", :646). The shipped Showroom scene has THREE shadow-casting
//     lamps: one of them has never cast a shadow, and which one changes as the
//     editor camera moves. The count is now derived from the scene
//     (OgreEngine::deriveShadowMapCount).
//
//  2. THE LAYOUT. The helper takes explicit atlasStart offsets, so growing the
//     count meant growing one vertical strip: at a 4096 base a third focused map
//     is 18432 rows, past the 16384 limit every Vulkan device and Metal report
//     (VulkanRenderSystem: min(maxImageDimension2D, 16384)). The layout is now
//     PACKED into columns (planShadowAtlas), and N == 2 reproduces the historical
//     R x 3.5R strip byte for byte — which is what keeps every existing pixel
//     suite unchanged.
//
//  3. STATIC MAPS. Upstream's helper emits ONE PASS_CLEAR per atlas, and on
//     Vulkan a clear is a render-pass load action over the WHOLE attachment
//     (VkRenderPassBeginInfo.renderArea = 0,0,targetWidth,targetHeight —
//     OgreVulkanRenderPassDescriptor::performLoadActions). That is why upstream
//     says "do not put static and dynamic shadow maps in the same UV atlas"
//     (OgreCompositorShadowNode.h:305) and why its own sample puts statics in a
//     second `keep_content` texture. A second texture is not free for us: with
//     `setStaticBranchingLights(true)` (a measured 4->2 shader-compile halving,
//     LIGHTS_SPEC F-L2) the PBS pixel shader samples ONE shadow texture index
//     for every point/spot map, fixed at compile time
//     (ShadowMapping_piece_ps.any:535-548). So the atlas must stay single and
//     the whole-atlas clear has to go: each map is cleared by its own QUAD pass
//     instead (Jahshaka/ShadowMapClear, media/Hlms/Jahshaka/JahshakaShadow.material),
//     gated on that map's index, exactly like the point-light DPSM copy already
//     writes one map's rectangle of this same atlas.
//
// WHAT IS COPIED AND WHAT IS NOT. The pass emission below is our own, written
// against the same PUBLIC definition API the helper uses (addTextureDefinition /
// addShadowTextureDefinition / addTargetPass / addPass). It deliberately
// reproduces upstream's parameter choices where we have no opinion (texture
// formats, cubemap size, per-face clear colours, the DPSM copy), so a diff
// against createShadowNodeWithSettings stays readable. It is a copy we own: an
// upstream change to the helper does not reach it.
#include "EnginePrivate.h"
#include "AtomPass.h"

#include <Compositor/Pass/PassClear/OgreCompositorPassClearDef.h>
#include <Compositor/Pass/PassQuad/OgreCompositorPassQuadDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreDepthBuffer.h>

#include <limits>       // keepNearestLamps' "unplaceable lamp" distance

namespace jahshaka { namespace engine { namespace detail {

// ---------------------------------------------------------------------------
// The packer
// ---------------------------------------------------------------------------
// One texture holds everything (see 3 above): the PSSM block in column 0 —
// split 0 at R x R with splits 1 and 2 at R/2 beneath it, i.e. a 1.5R tall
// header — and then the focused maps, R x R each, filling column 0 and then
// further columns to the right. Both dimensions are bounded by `maxDim`.
//
// The N == 2 case is load-bearing: it must reproduce the layout this engine has
// always built (one column: PSSM at y=0, focused at y=1.5R and y=2.5R), because
// that is what makes "a scene with at most two shadow casters renders exactly
// the bytes it rendered before" true by construction rather than by measurement.
ShadowAtlasPlan planShadowAtlas(unsigned baseResolution, unsigned focusedMaps, unsigned maxDim)
{
    ShadowAtlasPlan plan;
    const unsigned R = std::max(256u, baseResolution);
    const unsigned H = std::max(128u, R / 2u);
    plan.resolution = R;
    plan.focusedMaps = focusedMaps;
    // The PSSM header, unchanged from the historical layout.
    plan.pssm[0] = { 0u, 0u, R, R };
    plan.pssm[1] = { 0u, R, H, H };
    plan.pssm[2] = { H,  R, H, H };
    const unsigned header = R + H;                       // 1.5R

    // How many focused maps a column can hold: column 0 pays for the header.
    const unsigned capFirst = maxDim > header ? (maxDim - header) / R : 0u;
    const unsigned capOther = maxDim / R;
    if (capFirst == 0u && capOther == 0u) {              // R alone busts the cap
        plan.width = std::min(maxDim, R);
        plan.height = std::min(maxDim, header);
        plan.focusedMaps = 0u;
        return plan;
    }

    // Pick the column count and the split between column 0 and the rest that
    // minimises the allocated area (then the height, then the column count).
    unsigned bestCols = 0u, bestFirst = 0u, bestOther = 0u;
    unsigned long long bestArea = 0ull;
    unsigned bestHeight = 0u;
    for (unsigned cols = 1u; cols <= 16u; ++cols) {
        if (cols * R > maxDim) break;
        const unsigned firstCap = std::min(capFirst, focusedMaps);
        for (unsigned first = 0u; first <= firstCap; ++first) {
            const unsigned rest = focusedMaps - first;
            unsigned perOther = 0u;
            if (cols == 1u) { if (rest > 0u) continue; }
            else {
                perOther = (rest + (cols - 1u) - 1u) / (cols - 1u);   // ceil
                if (perOther > capOther) continue;
            }
            const unsigned h = std::max(header + first * R, perOther * R);
            if (h > maxDim) continue;
            const unsigned w = cols * R;
            const unsigned long long area = (unsigned long long)w * h;
            const bool better = bestCols == 0u || area < bestArea ||
                                (area == bestArea && h < bestHeight);
            if (better) { bestArea = area; bestHeight = h; bestCols = cols;
                          bestFirst = first; bestOther = perOther; }
        }
    }
    if (bestCols == 0u) {
        // The request does not fit this device at this resolution (16 maps at a
        // 4096 base want more texels than a 16384^2 texture has). Give back the
        // largest count that DOES fit rather than a token two: the caller is
        // told what it got through the plan, and `world.shadowStatus()` reports
        // it as the effective budget.
        for (unsigned n = focusedMaps; n-- > 0u; ) {
            const ShadowAtlasPlan smaller = planShadowAtlas(baseResolution, n, maxDim);
            if (smaller.focusedMaps == n) return smaller;
        }
        plan.width = std::min(maxDim, R);
        plan.height = std::min(maxDim, header);
        plan.focusedMaps = 0u;
        return plan;
    }

    plan.width = bestCols * R;
    plan.height = bestHeight;
    plan.focused.reserve(plan.focusedMaps);
    unsigned placed = 0u;
    for (unsigned i = 0u; i < bestFirst && placed < plan.focusedMaps; ++i, ++placed)
        plan.focused.push_back({ 0u, header + i * R, R, R });
    for (unsigned c = 1u; c < bestCols && placed < plan.focusedMaps; ++c)
        for (unsigned i = 0u; i < bestOther && placed < plan.focusedMaps; ++i, ++placed)
            plan.focused.push_back({ c * R, i * R, R, R });
    // The plan is what the definition below is built from; the height above is
    // an upper bound, so trim it to what was actually placed.
    unsigned used = 0u;
    for (const ShadowMapRect &r : plan.focused) used = std::max(used, r.y + r.h);
    plan.height = std::max(header, used);
    return plan;
}

unsigned long long OgreEngine::probeShadowNodeBytes() const {
    const Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    const unsigned maxDim = std::min(16384u, unsigned(rs && rs->getCapabilities()
                                                          ? rs->getCapabilities()->getMaximumResolution2D()
                                                          : 16384u));
    const unsigned probeRes = probeShadowResolution(mShadowResolution);
    const ShadowAtlasPlan plan =
        planShadowAtlas(probeRes, std::min(mShadowMapCount, kProbeShadowMaxFocusedMaps), maxDim);
    unsigned long long bytes = plan.bytes();
    if (plan.focusedMaps) {
        // buildShadowNode's tmpCubemap: max(64, R/2) square, six faces, R32F.
        const unsigned long long c = std::max(64u, probeRes / 2u);
        bytes += 6ull * c * c * 4ull;
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// The clear material
// ---------------------------------------------------------------------------
// Which of the two variants writes "far" depends on the backend: Ogre's own
// clear passes ask for depth 1.0 and the Vulkan render system stores
// `1.0 - clearDepth` under reverse depth (OgreVulkanRenderPassDescriptor.cpp:
// 463-468), so the far plane is 0.0 there and 1.0 everywhere else. Picking the
// material here keeps that decision in ONE place; the shader carries no uniform
// (a const-buffer parameter on a quad that runs once per map per frame is
// needless plumbing, and a wrong value would show up as shadow acne, not as an
// error).
const char *OgreEngine::shadowClearMaterialName() const
{
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    return (rs && rs->isReverseDepth()) ? "Jahshaka/ShadowMapClearRev"
                                        : "Jahshaka/ShadowMapClear";
}

// ---------------------------------------------------------------------------
// The definition
// ---------------------------------------------------------------------------
void OgreEngine::buildShadowNode(const char *name, unsigned baseResolution, unsigned focusedMaps,
                                 bool perMapClears, unsigned cubeResolution)
{
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    const Ogre::RenderSystemCapabilities *caps = rs->getCapabilities();
    unsigned maxDim = caps ? unsigned(caps->getMaximumResolution2D()) : 16384u;
    if (maxDim == 0u) maxDim = 16384u;
    maxDim = std::min(maxDim, 16384u);

    // `focusedMaps` is taken LITERALLY: every caller passes >= 2 today
    // (rebuildShadowAtlas clamps, and two is the floor the N == 2 layout
    // guarantee rests on); 0 is a valid PSSM-only node — R x 1.5R, no focused
    // column, no scratch cube — kept because it is the shape the probe node's
    // A/B was measured against (OgreView::kProbeShadowNodeName).
    const ShadowAtlasPlan plan = planShadowAtlas(baseResolution, focusedMaps, maxDim);
    const unsigned N = plan.focusedMaps;

    Ogre::CompositorShadowNodeDef *def = cm->addShadowNodeDefinition(name);

    // ---- textures -----------------------------------------------------
    // The atlas: one depth texture with an explicit RTV, exactly as the helper
    // declares it (a depth format + NO_POOL_EXPLICIT_RTV is what makes the
    // shadow maps SAMPLEABLE depth rather than a pooled depth buffer).
    def->setNumLocalTextureDefinitions(N ? 2u : 1u);   // atlas0 (+ tmpCubemap)
    const Ogre::String atlasName = "atlas0";
    {
        Ogre::TextureDefinitionBase::TextureDefinition *texDef = def->addTextureDefinition(atlasName);
        texDef->width  = std::max(1u, plan.width);
        texDef->height = std::max(1u, plan.height);
        texDef->format = Ogre::PFG_D32_FLOAT;
        texDef->depthBufferId = Ogre::DepthBuffer::NO_POOL_EXPLICIT_RTV;
        texDef->depthBufferFormat = Ogre::PFG_D32_FLOAT;
        texDef->preferDepthTexture = false;
        texDef->fsaa = "1";
        // A CACHING ATLAS KEEPS ITS CONTENT (keep_content; ENGINE_CACHE_POLICY
        // E2 probe, 2026-09-12). Compositor textures are born DiscardableContent,
        // which tells the backend it may throw the texels away between uses —
        // the opposite of a cached lamp map, which must survive every frame it is
        // not redrawn. It is also a hard failure, not only a theoretical one: in
        // a frame where every pass of a node is skipped (all its lamps cached and
        // clean, no sun), the first touch of the atlas is the scene pass that
        // SAMPLES it, and the barrier solver refuses a discardable texture's
        // first transition being a read — "Transitioning texture from Undefined
        // to a read-only layout ... keep_content" (OgreResourceTransition.cpp:
        // 185). That exception aborted the pass after the solver had already
        // recorded the pass's colour target as a render target, so the NEXT
        // frame's barrier read COLOR_ATTACHMENT -> COLOR_ATTACHMENT over an image
        // that was really SHADER_READ: a Vulkan validation error on the planar
        // mirror's and the probes' targets (tests/shadow `cachekinds`). Upstream's
        // own StaticShadowMaps sample declares its static atlas keep_content for
        // this reason. The whole-atlas-clear shape (nothing cached) keeps the
        // default, so a scene without point/spot casters builds exactly the
        // definition it always did.
        if (perMapClears) texDef->textureFlags &= ~Ogre::uint32(Ogre::TextureFlags::DiscardableContent);
        Ogre::RenderTargetViewDef *rtv = def->addRenderTextureView(atlasName);
        rtv->setForTextureDefinition(atlasName, texDef);
    }
    // The point-light scratch cubemap (six faces rendered, then copied into the
    // light's rectangle of the atlas as a dual-paraboloid map). Only when a
    // focused map exists to copy into: at the historical 1024 it is 24 MB PER
    // NODE INSTANCE whatever the atlas resolution (the probe node passes R/2).
    const Ogre::String cubeName = "tmpCubemap";
    if (N) {
        Ogre::TextureDefinitionBase::TextureDefinition *texDef = def->addTextureDefinition(cubeName);
        texDef->width  = std::max(64u, cubeResolution);
        texDef->height = std::max(64u, cubeResolution);
        texDef->depthOrSlices = 6u;
        texDef->textureType = Ogre::TextureTypes::TypeCube;
        texDef->format = Ogre::PFG_R32_FLOAT;
        texDef->depthBufferId = 1u;
        texDef->depthBufferFormat = Ogre::PFG_D32_FLOAT;
        texDef->preferDepthTexture = false;
        Ogre::RenderTargetViewDef *rtv = def->addRenderTextureView(cubeName);
        rtv->setForTextureDefinition(cubeName, texDef);
    }

    // ---- the shadow maps ----------------------------------------------
    // Light 0 is the directional light with its three PSSM splits; lights
    // 1..N are the focused point/spot maps, one light each.
    const unsigned numMaps = 3u + N;
    def->setNumShadowTextureDefinitions(numMaps);
    const Ogre::Vector2 texSize(Ogre::Real(plan.width), Ogre::Real(plan.height));
    const auto addMap = [&](size_t lightIdx, size_t split, const ShadowMapRect &r,
                            Ogre::ShadowMapTechniques technique, Ogre::uint32 numSplits) {
        Ogre::ShadowTextureDefinition *td = def->addShadowTextureDefinition(
            lightIdx, split, atlasName,
            Ogre::Vector2(Ogre::Real(r.x), Ogre::Real(r.y)) / texSize,
            Ogre::Vector2(Ogre::Real(r.w), Ogre::Real(r.h)) / texSize, 0u);
        td->shadowMapTechnique = technique;
        td->xyPadding       = kShadowXyPadding;
        td->pssmLambda      = kPssmLambda;
        td->splitPadding    = kPssmSplitPadding;
        td->splitBlend      = kPssmSplitBlend;
        td->splitFade       = kPssmSplitFade;
        td->numSplits       = numSplits;
        td->numStableSplits = kPssmStableSplits;
        // THE SUN'S CONSTANT BIAS (PHOTON-RAYS-1 fix round, F3). Upstream leaves
        // it at HlmsDatablock::mShadowConstantBias 0.01 x this scale 1.0, applied
        // in WORLD UNITS along the light (ShadowCaster_piece_vs.any: bias x
        // depthRange.y), then x the split's auto factor 1 + 100 x orthoSize /
        // shadowFarDistance — 4-18 cm at the splits a 5-30 m view uses, which is
        // the light the map left under every thin caster (gi.sun_contact's board:
        // 44 mm at 5.2 m, 157 mm under 5 cm at 15 m, 165-185 mm at 30 m). A D32
        // atlas does not need it; the slope is the receiver's normal-offset bias's
        // to carry (168 texels x its auto factor, untouched).
        //   scale  board leak 5.2 / 15 / 30 m    acne, selftest fixture B's glossy
        //                                         plane under a 52-degree sun
        //   1.0    6.0 / 23.6 / 25-28 px          none (the old picture)
        //   0.5    1.5 /  7.6-8.1 / 12-13 px      38 px moved, 5 > 4 codes
        //   0.3    0.0 /  1.3-1.7 /  6.1-6.6 px   58 px moved, 9 > 4 codes (edges)
        //   0.2    —                              1,150 px moved, 23 > 4: faint rings
        //   0.1    0.0 /  0.0 / 0.0-0.2 px        3,428 px moved, 333 > 4: visible acne
        // (the matte floor and the sphere lattice show no acne down to 0.01; the
        // glossy plane is the bound). 0.3 = the lowest value without visible acne.
        // Directional (PSSM) maps only: the focused point/spot maps were not
        // measured and keep upstream's.
        if (technique == Ogre::SHADOWMAP_PSSM) td->constantBiasScale = kPssmConstantBiasScale;
    };
    for (unsigned j = 0u; j < 3u; ++j)
        addMap(0u, j, plan.pssm[j], Ogre::SHADOWMAP_PSSM, 3u);
    for (unsigned i = 0u; i < N; ++i)
        addMap(1u + i, 0u, plan.focused[i], Ogre::SHADOWMAP_FOCUSED, 1u);

    // ---- the passes ---------------------------------------------------
    //   the clears: ONE whole-atlas clear, or one quad per map     (1 or 3 + N)
    //   PSSM:    one scene pass per split                         (3)
    //   focused: one scene pass (spot) + 6 cube faces + 1 copy     (N * 8)
    def->setNumTargetPass((perMapClears ? numMaps : 1u) + 3u + N * 8u);

    // THE ATOM CASTERS (ATOM-SHADOWS-1; OgreAtomCasterPass.cpp): where this device
    // runs the visibility buffer, every caster scene pass SKIPS the Atom queue and is
    // followed, on the same target and map, by the caster cut's PASS_CUSTOM — the
    // light's own cluster cut, one indirect command per instance, depth only. A
    // device without the id pass (the Mac, the NULL render system) never files an
    // item there and keeps upstream's pass list exactly.
    const bool atomCasters = atomIdPassSupported(rs);
    const auto addAtomCaster = [&](Ogre::CompositorTargetDef *target, Ogre::CompositorPassSceneDef *scene) {
        if (!atomCasters) return;
        scene->setSkipRenderQueue(kAtomRenderQueue, true);
        Ogre::CompositorPassDef *p = target->addPass(Ogre::PASS_CUSTOM, Ogre::IdString(kAtomCasterPassId));
        p->setAllLoadActions(Ogre::LoadAction::Load);
        p->mStoreActionColour[0] = Ogre::StoreAction::Store;
        p->mStoreActionDepth = Ogre::StoreAction::Store;
        p->mStoreActionStencil = Ogre::StoreAction::DontCare;
        p->mShadowMapIdx = scene->mShadowMapIdx;
        p->mIncludeOverlays = false;
        p->mProfilingId = "Jahshaka atom caster";
    };

    // What this node's maps draw: ONE definition shared with the lamp-map
    // cache's caster scan (shadowCasterChannels — R1/R2 widen it per kind).
    const ShadowNodeKind kind =
        Ogre::IdString(name) == Ogre::IdString(OgreView::kReflectShadowNodeName) ? ShadowNodeKind::Reflect
        : Ogre::IdString(name) == Ogre::IdString(OgreView::kProbeShadowNodeName) ? ShadowNodeKind::Probe
                                                                                 : ShadowNodeKind::View;
    // THE STOCK CASTER'S CHANNEL (kStockCasterBit, ENGINE-POLISH-ATOM-1 stage 2): the scene passes draw the casters the
    // caster cut does not (an item whose caster the cut draws drops the pair); within the reserved bits (trap 6).
    const Ogre::uint32 casterMask = shadowPassMask(kind);
    const Ogre::uint8 directionalMask = Ogre::uint8(1u << Ogre::Light::LT_DIRECTIONAL);
    const Ogre::uint8 pointMask       = Ogre::uint8(1u << Ogre::Light::LT_POINT);
    const Ogre::uint8 spotMask        = Ogre::uint8(1u << Ogre::Light::LT_SPOTLIGHT);
    const char *clearMaterial = shadowClearMaterialName();

    // (a) THE CLEAR. Upstream's ONE whole-atlas clear while no drawn scene in
    //     this process holds a cacheable (point/spot, shadowed) lamp, and one
    //     QUAD PER MAP once one does — because the quads are not free and,
    //     until a lamp map is cached, they buy nothing.
    //
    //     MEASURED, which is why this is a switch and not a constant
    //     (gi.coalesce, 2026-09-09): three quads over the PSSM block (2048^2 +
    //     two 1024^2 of depth writes) every frame, where a hardware fast-clear
    //     had covered the whole atlas, delayed the frame enough to flip a
    //     neighbouring GI suite's frame-phase 3 times in 6 runs. With this
    //     switch a scene lit only by the sun executes exactly upstream's pass
    //     list again. (ENGINE_CACHE_POLICY_SPEC risk 3: the lamp-map cache makes
    //     the quads the NORMAL case wherever lamps cast — if a frame-phase suite
    //     moves again, the fix belongs on the test's side.)
    //
    //     The engine rebuilds the node when the first cacheable lamp appears
    //     (the same drop-swap-recreate a Shadow Quality change costs), and
    //     never back within the session (applyShadowCache says why).
    if (!perMapClears) {
        Ogre::CompositorTargetDef *target = def->addTargetPass(atlasName);
        target->setNumPasses(1u);
        Ogre::CompositorPassDef *passDef = target->addPass(Ogre::PASS_CLEAR);
        Ogre::CompositorPassClearDef *clr = static_cast<Ogre::CompositorPassClearDef *>(passDef);
        clr->setAllClearColours(shadowClearColour());
        clr->mClearDepth = 1.0f;
    }
    // (a2) THE PER-MAP CLEARS, in map order. Each one is gated by
    //     CompositorNode::_update on ITS OWN map index: it runs when the map
    //     holds a light and that light is dynamic or a dirty static
    //     (_shouldUpdateShadowMapIdx), which is precisely the frames in which
    //     the map is about to be re-rendered. A cached map that is clean is
    //     neither cleared nor drawn, so its contents survive — the whole point.
    for (unsigned m = 0u; perMapClears && m < numMaps; ++m) {
        Ogre::CompositorTargetDef *target = def->addTargetPass(atlasName);
        target->setShadowMapSupportedLightTypes(m < 3u ? directionalMask
                                                       : Ogre::uint8(pointMask | spotMask));
        target->setNumPasses(1u);
        Ogre::CompositorPassDef *passDef = target->addPass(Ogre::PASS_QUAD);
        Ogre::CompositorPassQuadDef *quad = static_cast<Ogre::CompositorPassQuadDef *>(passDef);
        quad->mMaterialIsHlms = false;
        quad->mMaterialName = clearMaterial;
        quad->mShadowMapIdx = m;
    }

    // (b) The directional splits.
    for (unsigned j = 0u; j < 3u; ++j) {
        Ogre::CompositorTargetDef *target = def->addTargetPass(atlasName);
        target->setShadowMapSupportedLightTypes(directionalMask);
        target->setNumPasses(atomCasters ? 2u : 1u);
        Ogre::CompositorPassSceneDef *scene =
            static_cast<Ogre::CompositorPassSceneDef *>(target->addPass(Ogre::PASS_SCENE));
        scene->mShadowMapIdx = j;
        scene->mFirstRQ = 0u;
        scene->mLastRQ = 255u;
        scene->mIncludeOverlays = false;
        scene->mVisibilityMask = casterMask;
        addAtomCaster(target, scene);
    }

    // (c) The focused maps: a spot pass, then the point-light cubemap + copy.
    //     Upstream renders every directional/spot pass before every point pass;
    //     the order between maps does not matter here because each map clears
    //     itself, but keeping it makes the two definitions comparable.
    for (unsigned i = 0u; i < N; ++i) {
        Ogre::CompositorTargetDef *target = def->addTargetPass(atlasName);
        target->setShadowMapSupportedLightTypes(spotMask);
        target->setNumPasses(atomCasters ? 2u : 1u);
        Ogre::CompositorPassSceneDef *scene =
            static_cast<Ogre::CompositorPassSceneDef *>(target->addPass(Ogre::PASS_SCENE));
        scene->mShadowMapIdx = 3u + i;
        scene->mFirstRQ = 0u;
        scene->mLastRQ = 255u;
        scene->mIncludeOverlays = false;
        scene->mVisibilityMask = casterMask;
        addAtomCaster(target, scene);
    }
    for (unsigned i = 0u; i < N; ++i) {
        for (Ogre::uint32 face = 0u; face < 6u; ++face) {
            Ogre::CompositorTargetDef *target = def->addTargetPass(cubeName, face);
            target->setShadowMapSupportedLightTypes(pointMask);
            target->setNumPasses(atomCasters ? 2u : 1u);
            Ogre::CompositorPassSceneDef *scene =
                static_cast<Ogre::CompositorPassSceneDef *>(target->addPass(Ogre::PASS_SCENE));
            scene->setAllLoadActions(Ogre::LoadAction::Clear);
            scene->setAllClearColours(shadowClearColour());
            scene->mClearDepth = 1.0f;
            scene->mCameraCubemapReorient = true;
            scene->mShadowMapIdx = 3u + i;
            scene->mFirstRQ = 0u;
            scene->mLastRQ = 255u;
            scene->mIncludeOverlays = false;
            scene->mVisibilityMask = casterMask;
            addAtomCaster(target, scene);
        }
        Ogre::CompositorTargetDef *target = def->addTargetPass(atlasName);
        target->setShadowMapSupportedLightTypes(pointMask);
        target->setNumPasses(1u);
        Ogre::CompositorPassQuadDef *quad =
            static_cast<Ogre::CompositorPassQuadDef *>(target->addPass(Ogre::PASS_QUAD));
        quad->mMaterialIsHlms = false;
        quad->mMaterialName = "Ogre/DPSM/CubeToDpsm";
        quad->addQuadTextureSource(0, cubeName);
        quad->mShadowMapIdx = 3u + i;
    }
}

// The colour the cube faces clear to, by the same rule the helper uses: white
// unless the backend is reverse-depth (OgreCompositorShadowNode.cpp:1039-1044).
Ogre::ColourValue OgreEngine::shadowClearColour() const
{
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    if (rs && rs->isReverseDepth()) return Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.0f);
    return Ogre::ColourValue::White;
}

// ---------------------------------------------------------------------------
// The global knobs the atlas is built from
// ---------------------------------------------------------------------------
void OgreEngine::setShadowFilter(ShadowFilter f) {
    // THE HOST'S CALL IS A MESSAGE (U3 stage 4a, decision 39), applied at the drain; JAH_HOST_CALL counts a direct one.
    if (hostPosts()) mHostShadowFilter = f;   // the host's record, at the call
    if (postedAsMessage([=](OgreEngine &e) { e.setShadowFilter(f); return true; })) return;
    JAH_HOST_CALL("Engine::setShadowFilter");
    mShadowFilter = f;
    if (!mHostRecords) mHostShadowFilter = f;   // the host's record (Engine::shadowFilter); at its call when records (D5)
    if (mHlmsRegistered) applyShadowFilter();
    // else: applied by ensureHlms() once the Hlms exists.
}


/// Maps the neutral enum onto HlmsPbs. PCF only — see the declaration.
void OgreEngine::applyShadowFilter() {
    JAH_TRY {
        auto *pbs = static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        if (!pbs) return;
        Ogre::HlmsPbs::ShadowFilter f = Ogre::HlmsPbs::PCF_4x4;
        switch (mShadowFilter) {
        case ShadowFilter::Hard:     f = Ogre::HlmsPbs::PCF_2x2; break;
        case ShadowFilter::Soft:     f = Ogre::HlmsPbs::PCF_4x4; break;
        case ShadowFilter::VerySoft: f = Ogre::HlmsPbs::PCF_6x6; break;
        }
        pbs->setShadowSettings(f);
    } JAH_CATCH(mLastError, );
}

void OgreEngine::setShadowResolution(unsigned pixels) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (hostPosts()) mHostShadowResolution = std::min(8192u, std::max(256u, pixels));   // the host's record, at the call
    if (postedAsMessage([=](OgreEngine &e) { e.setShadowResolution(pixels); return true; })) return;
    const unsigned res = std::min(8192u, std::max(256u, pixels));
    if (res == mShadowResolution) return;
    rebuildShadowAtlas(res, mShadowMapCount, mShadowPerMapClears);
}


/// THE REBUILD. Both the resolution and the map count live in the shadow-node
/// DEFINITION, which Ogre refuses to replace while anything instantiates it —
/// so every workspace that names the node has to be dropped, the definitions
/// swapped, and the workspaces re-created. That is the same teardown order the
/// engine's destructor honours, and it is the reason the count is stepped
/// {2,4,8,16} rather than tracked exactly: this is not a per-frame operation.
///
/// Returns false when nothing was done (values unchanged, or no Hlms yet — in
/// which case the first createShadowNode() will pick the new values up).
bool OgreEngine::rebuildShadowAtlas(unsigned resolution, unsigned focusedMaps, bool clears) {
    if (mHeadless) return false;   // no pixels, no atlases (the siblings gate the same way)
    const unsigned res = std::min(8192u, std::max(256u, resolution));
    const unsigned maps = std::min(kMaxShadowMaps, std::max(2u, focusedMaps));
    if (res == mShadowResolution && maps == mShadowMapCount && clears == mShadowPerMapClears &&
        mHlmsRegistered)
        return false;
    mShadowResolution = res;
    mHostShadowResolution = res;   // the boundary's record (Engine::shadowResolution)
    mShadowMapCount = maps;
    mShadowPerMapClears = clears;
    if (!mHlmsRegistered) return false;   // first createShadowNode() picks it up
    // THE MONITOR'S ATLAS EVENT. An atlas change drops and recreates EVERY
    // workspace that names a shadow node — the views', the planar mirrors' and
    // the reflection probes', whose GI arm is rebuilt from scratch — so it is
    // both a hitch and the reason a listener list can vanish under the monitor.
    if (monitor::live())   // §4.1 is ABSOLUTE: build no string while off
        monitor::noteEvent(MonitorEventKind::AtlasRebuild, WorkReason::Resolution, "shadow.atlas",
                           std::to_string(res) + "px, " + std::to_string(maps) + " focused maps");
    bool ok = false;
    JAH_TRY {
        Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
        std::vector<OgreView *> rebuilt;
        for (auto &v : mViews)
            if (v->dropWorkspaceForShadowRebuild()) rebuilt.push_back(v.get());
        // The planar-reflection arm instantiates the HALF-resolution shadow node
        // in each of its private workspaces, so it holds the same kind of
        // reference a view's workspace does and must be dropped for the same
        // reason. Scenes whose reflections do not use shadows report false.
        std::vector<OgreScene *> planarRebuilt;
        for (auto &s : mScenes)
            if (s->dropPlanarForShadowRebuild()) planarRebuilt.push_back(s.get());
        // ...and the hybrid's SHADOWED probe captures, which instantiate the
        // same node inside each probe workspace (risk R3 — a SEGV, reproduced).
        std::vector<OgreScene *> giRebuilt;
        for (auto &s : mScenes)
            if (s->dropGiForShadowRebuild()) giRebuilt.push_back(s.get());
        if (cm->hasShadowNodeDefinition(OgreView::kShadowNodeName))
            cm->removeShadowNodeDefinition(OgreView::kShadowNodeName);
        if (cm->hasShadowNodeDefinition(OgreView::kReflectShadowNodeName))
            cm->removeShadowNodeDefinition(OgreView::kReflectShadowNodeName);
        if (cm->hasShadowNodeDefinition(OgreView::kProbeShadowNodeName))
            cm->removeShadowNodeDefinition(OgreView::kProbeShadowNodeName);
        createShadowNode();
        for (OgreView *v : rebuilt) v->recreateWorkspaceAfterShadowRebuild();
        for (OgreScene *s : planarRebuilt) s->recreatePlanarAfterShadowRebuild();
        for (OgreScene *s : giRebuilt) s->recreateGiAfterShadowRebuild();
        ++mShadowAtlasRebuilds;                 // ShadowStatus::atlasRebuilds
        ok = true;
    } JAH_CATCH(mLastError, false);
    return ok;
}

// ---------------------------------------------------------------------------
// The derived count (SHADOW_TOOLING_SPEC.md §4.1)
// ---------------------------------------------------------------------------
void OgreEngine::setShadowMapBudget(unsigned maps) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { e.setShadowMapBudget(maps); return true; })) return;
    const unsigned want = std::min(kMaxShadowMaps, std::max(2u, maps));
    if (want == mShadowMapBudget) return;
    mShadowMapBudget = want;
    // Lowering the budget does NOT shrink an atlas that already grew (owner
    // decision D4: never shrink in-session). It takes effect the next time the
    // count would have grown, and shadowStatus() reports it immediately.
    mDerivedShadowMapWant = 0;
    mDerivedShadowMapFrames = 0;
    mWarnedShadowCasters = 0;
}

unsigned OgreEngine::shadowMapBudget() const { return mShadowMapBudget; }

unsigned OgreEngine::effectiveShadowMapBudget() const {
    // The resolution cap. It is not a policy preference — it is the packer's
    // arithmetic stated up front: at a 4096 base only 14 maps fit inside a
    // 16384^2 texture at all, and 8 of them already cost 672 MB. The tiers the
    // host ships (2/4/8/8 at 512/1024/2048/2048) sit inside these numbers, so
    // the cap only ever bites a hand-set resolution.
    unsigned cap = 2u;
    if (mShadowResolution <= 1024u)      cap = kMaxShadowMaps;
    else if (mShadowResolution <= 2048u) cap = 8u;
    else if (mShadowResolution <= 4096u) cap = 4u;
    return std::max(2u, std::min(std::min(mShadowMapBudget, cap), kMaxShadowMaps));
}

namespace {
/// {2, 4, 8, 16}: the step allocation. Head-room is free in shader terms (Ogre
/// keys its permutations on ACTIVE casters, OgreHlms.cpp:3268-3300), and each
/// step costs one rebuild, so a scene that grows a lamp at a time pays at most
/// three rebuilds for its whole session instead of one per lamp.
unsigned stepShadowMapCount(unsigned casters) {
    if (casters <= 2u) return 2u;
    if (casters <= 4u) return 4u;
    if (casters <= 8u) return 8u;
    return kMaxShadowMaps;
}
}   // namespace

unsigned OgreEngine::shadowMapDemand(unsigned *castersOut) {
    if (castersOut) *castersOut = 0;
    if (!mHlmsRegistered || mHeadless) return mShadowMapCount;
    // What the frame is about to draw — the same set the frame loop updates, so
    // a preview scene nobody is looking at cannot force the editor's atlas to
    // grow.
    std::vector<OgreScene *> &scenes = mShadowSceneScratch;   // per frame; clean-2 lane
    scenesFeedingEnabledViews(scenes);
    unsigned casters = 0;
    for (OgreScene *s : scenes) casters = std::max(casters, s->countLocalShadowCasters(nullptr));
    if (castersOut) *castersOut = casters;

    const unsigned budget = effectiveShadowMapBudget();
    // THE EXCEEDED CASE, SAID OUT LOUD ONCE — and said from HERE, so both paths
    // that can grow the atlas keep the same bookkeeping (F3, round 2). Ogre's
    // own behaviour (keep the closest casters, drop the rest) is unchanged;
    // what changes is that it stops being silent. The host repeats it in the
    // World panel and through shadowStatus().
    if (casters > budget && casters != mWarnedShadowCasters) {
        mWarnedShadowCasters = casters;
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka shadows: " + Ogre::StringConverter::toString(casters) +
            " shadow-casting point/spot lights but only " +
            Ogre::StringConverter::toString(budget) +
            " shadow maps are budgeted; the farthest lights render no shadow. "
            "Raise the Shadow Map Budget or turn off Cast Shadows on distant lights.");
    } else if (casters <= budget) {
        mWarnedShadowCasters = 0;
    }
    return std::min(stepShadowMapCount(casters), budget);
}

/// The ONE line either growth path writes, so a log tells you which frame the
/// atlas grew and why, whichever path did it (F3, round 2).
void OgreEngine::logShadowAtlasGrowth(unsigned want, unsigned casters, const char *why) {
    Ogre::LogManager::getSingleton().logMessage(
        "Jahshaka shadows: growing the atlas to " + Ogre::StringConverter::toString(want) +
        " focused shadow maps for " + Ogre::StringConverter::toString(casters) +
        " casters (" + why + ")");
}

bool OgreEngine::anyDrawnViewPresented() {
    for (auto &v : mViews)
        if (v && v->isEnabled() && v->ogreScene() && v->framesPresented() > 0u) return true;
    return false;
}

void OgreEngine::deriveShadowMapCount() {
    if (!mHlmsRegistered || mHeadless) return;
    // The scan, the over-budget warning and the stepped demand all live in
    // shadowMapDemand now, so the flip path below cannot drift from this one.
    unsigned casters = 0;
    const unsigned want = shadowMapDemand(&casters);

    if (want <= mShadowMapCount) {          // never shrink in-session (D4)
        mDerivedShadowMapWant = 0;
        mDerivedShadowMapFrames = 0;
        return;
    }
    // ...AND IT WAITS FOR THE WORLD TO BE UP, exactly as the clear-strategy
    // flip beside it does (F1, round 2). Both of them REBUILD THE ATLAS, and a
    // rebuild drops and recreates every workspace that names a shadow node
    // while the texture streamer is still uploading into its pooled
    // Type2DArrays — the race the sun lane measured on 2026-09-13 as 3-10
    // `VUID-vkCmdDraw-None-09600` image-layout errors per boot. The flip was
    // moved out of that window then; this path was not, and it is the one that
    // fires FIRST, so the window stayed open for every world that opens with
    // three or more casting lamps.
    //
    // It also makes the coalescing SYMMETRIC, which was the defect. The flip
    // publishes `mShadowClearFlipWanted` only while it is itself presenting;
    // a growth that fired before the first present carried `false`, rebuilt
    // with the old clear strategy, and left the flip to rebuild a second time
    // a few presented frames later — two rebuilds for one event, the exact
    // thing the coalescing exists to stop. Counting the debounce in PRESENTED
    // frames means that by the time this fires, applyShadowCache has published
    // a true answer at least twice.
    //
    // The predicate is anyDrawnViewPresented(), NOT the flip's `presenting`:
    // that one is computed inside the lamp scan and is false in a scene whose
    // casters are not cacheable, which would have stalled the growth for ever.
    if (!anyDrawnViewPresented()) {
        mDerivedShadowMapWant = 0;
        mDerivedShadowMapFrames = 0;
        return;
    }
    // DEBOUNCE. A scene loads its lights over many frames and every rebuild
    // drops and recreates every workspace that names the shadow node; waiting
    // for the demand to hold still turns "one hitch per lamp" into one hitch.
    if (want != mDerivedShadowMapWant) {
        mDerivedShadowMapWant = want;
        mDerivedShadowMapFrames = 1u;
        return;
    }
    if (++mDerivedShadowMapFrames < kShadowDeriveDebounceFrames) return;
    mDerivedShadowMapWant = 0;
    mDerivedShadowMapFrames = 0;
    logShadowAtlasGrowth(want, casters, "derived count");
    // ...AND THE CLEAR FLIP RIDES ALONG (the "first-lamp atlas hitch"). A
    // rebuild drops and recreates every workspace that names a shadow node —
    // the views', the mirrors', and every reflection probe's, whose GI arm is
    // built from scratch. A world opening with three or more casting lamps used
    // to pay that TWICE for one event: the growth here, and the clear-strategy
    // flip three frames later (applyShadowCache). Flipping inside a rebuild
    // that is happening anyway is free, and it is the SAME hazard window the
    // growth already opens — so the second rebuild simply stops existing.
    rebuildShadowAtlas(mShadowResolution, want, mShadowPerMapClears || mShadowClearFlipWanted);
    mShadowClearFlipFrames = 0;
}

// ---------------------------------------------------------------------------
// The readback
// ---------------------------------------------------------------------------
void OgreEngine::publishShadowStatus(unsigned frames) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { e.publishShadowStatus(frames); return true; })) return;
    mShadowLeaseUntil = frames ? mShadowFrame + frames : 0ull;
}

ShadowStatus OgreEngine::shadowStatus() const {
    JAH_SYNC_GETTER("Engine::shadowStatus");
    ShadowStatus st;
    st.requestedBudget = mShadowMapBudget;
    st.atlasRebuilds = mShadowAtlasRebuilds;        // cumulative; true even headless
    // The caster walk's item visits, summed over every live scene (F5). Outside
    // the poll gate below and before the headless return, like atlasRebuilds:
    // it is a plain counter, not a listener-fed one, and a headless host can
    // still read it.
    for (const auto &s : mScenes) st.casterWalkItems += s->casterWalkItems();
    if (!mHlmsRegistered || mHeadless) return st;   // live stays false
    // (THE COUNTERS RUN BY SUBSCRIPTION — publishShadowStatus, U3 stage 3; reading arms nothing.)
    JAH_TRY {
        st.resolution = mShadowResolution;
        st.budget = effectiveShadowMapBudget();
        st.pssmSplits = 3u;
        st.focusedMaps = mShadowMapCount;
        st.maps = st.pssmSplits + st.focusedMaps;
        const ShadowAtlasPlan plan = planShadowAtlas(
            mShadowResolution, mShadowMapCount,
            std::min(16384u, unsigned(mRoot->getRenderSystem()->getCapabilities()
                                          ? mRoot->getRenderSystem()->getCapabilities()->getMaximumResolution2D()
                                          : 16384u)));
        st.atlasWidth = plan.width;
        st.atlasHeight = plan.height;
        // THE MIRRORS' AND PROBES' ATLASES ARE REAL VRAM TOO, and D3 made the
        // mirror's grow with the lamp count: one half-resolution atlas per live
        // planar slot, one quarter-resolution atlas per shadowed probe — the
        // same definitions createShadowNode builds, counted per instance.
        {
            const unsigned maxDim = std::min(16384u, unsigned(mRoot->getRenderSystem()->getCapabilities()
                                        ? mRoot->getRenderSystem()->getCapabilities()->getMaximumResolution2D()
                                        : 16384u));
            unsigned long long reflectSlots = 0, probeSlots = 0;
            for (const auto &sc : mScenes) {
                std::vector<Ogre::CompositorWorkspace *> ws;
                sc->shadowWorkspaces(ShadowNodeKind::Reflect, ws);
                reflectSlots += ws.size();
                ws.clear();
                sc->shadowWorkspaces(ShadowNodeKind::Probe, ws);
                probeSlots += ws.size();
            }
            const unsigned probeRes = probeShadowResolution(mShadowResolution);
            st.reflectAtlasBytes = reflectSlots *
                planShadowAtlas(std::max(256u, mShadowResolution / 2u), mShadowMapCount, maxDim).bytes();
            st.probeAtlasBytes = probeSlots *
                planShadowAtlas(probeRes, std::min(mShadowMapCount, kProbeShadowMaxFocusedMaps), maxDim).bytes();
        }
        st.atlasBytes = plan.bytes() + st.reflectAtlasBytes;
        st.focusedMaps = plan.focusedMaps;
        st.maps = st.pssmSplits + st.focusedMaps;
        st.lightSlots = 1u + st.focusedMaps;
        st.live = true;
        st.countersMeasured = mShadowCountersArmed;
        st.shadowPassesLastFrame = mShadowPassesLastFrame;
        st.cachedMapRendersLastFrame = mCachedMapRendersLastFrame;
        st.shaderLightMismatches = FogHlmsListener::lightCountMismatches();
        st.reflectPassesLastFrame = mShadowKindPasses[unsigned(ShadowNodeKind::Reflect)];
        st.probePassesLastFrame = mShadowKindPasses[unsigned(ShadowNodeKind::Probe)];
        st.reflectLampPassesLastFrame = mShadowKindLampPasses[unsigned(ShadowNodeKind::Reflect)];
        st.probeLampPassesLastFrame = mShadowKindLampPasses[unsigned(ShadowNodeKind::Probe)];
        st.cachedInstances = mShadowCachedInstances[0] + mShadowCachedInstances[1] +
                             mShadowCachedInstances[2];
        st.uncachedInstances = mShadowUncachedInstances[0] + mShadowUncachedInstances[1] +
                               mShadowUncachedInstances[2];
        st.viewCached = mShadowCachedInstances[unsigned(ShadowNodeKind::View)] > 0u &&
                        mShadowUncachedInstances[unsigned(ShadowNodeKind::View)] == 0u;
        st.mapsDirtiedLastFrame = mShadowDirtiedMaps[0] + mShadowDirtiedMaps[1] +
                                  mShadowDirtiedMaps[2];

        // The demand, and who actually holds a map. The mapping is per WORKSPACE
        // (CompositorShadowNode is instance state), so it is read from the first
        // enabled view that has one — the editor's view in practice.
        std::vector<OgreScene *> scenes;
        scenesFeedingEnabledViews(scenes);
        std::vector<NodeId> casters;
        OgreScene *primary = nullptr;
        for (OgreScene *s : scenes) {
            std::vector<NodeId> ids;
            s->countLocalShadowCasters(&ids);
            if (ids.size() > casters.size()) { casters = ids; primary = s; }
        }
        st.casters = unsigned(casters.size());

        const Ogre::CompositorShadowNode *node = nullptr;
        for (const auto &v : mViews) {
            if (!v->isEnabled()) continue;
            if (const Ogre::CompositorShadowNode *n = v->shadowNodeInstance()) { node = n; break; }
        }
        if (node && primary) {
            const Ogre::LightClosestArray &lights = node->getShadowCastingLights();
            std::vector<NodeId> seen;
            for (size_t i = 0; i < lights.size(); ++i) {
                ShadowMapInfo info;
                info.slot = unsigned(i);
                info.pssm = (i == 0);   // light 0 is the directional PSSM set
                if (lights[i].light) {
                    info.node = primary->nodeOfLight(lights[i].light);
                    info.isCached = lights[i].isStatic;
                    info.dirty = lights[i].isDirty;
                    if (info.node) seen.push_back(info.node);
                }
                // The passes the last frame spent on this slot's map(s): three
                // PSSM rectangles for slot 0, one focused map (idx slot + 2)
                // for the others. Zero for a cached lamp at rest.
                if (i == 0) {
                    for (unsigned m = 0; m < 3u && m < mShadowViewMapPasses.size(); ++m)
                        info.passesLastFrame += mShadowViewMapPasses[m];
                } else if (i + 2u < mShadowViewMapPasses.size()) {
                    info.passesLastFrame = mShadowViewMapPasses[i + 2u];
                }
                st.mapped.push_back(info);
            }
            for (NodeId c : casters)
                if (std::find(seen.begin(), seen.end(), c) == seen.end()) st.unmapped.push_back(c);
        } else {
            // No live workspace to ask (nothing is drawing this scene yet): the
            // demand is still worth reporting, and every caster beyond the
            // allocation is by definition unmapped.
            for (size_t i = st.focusedMaps; i < casters.size(); ++i)
                st.unmapped.push_back(casters[i]);
        }
    } JAH_CATCH(mLastError, st);
    return st;
}

// ---------------------------------------------------------------------------
// THE LAMP-MAP CACHE, engine half (ENGINE_CACHE_POLICY_SPEC P2-P5, P8)
// ---------------------------------------------------------------------------
// Every point/spot map is CACHED: tied to its light with Ogre's own static
// shadow-map API (setLightFixedToShadowMap), rendered once, and re-rendered
// only when setStaticShadowMapDirty says so — which the scene's detection pass
// decides per light (OgreScene::collectShadowCacheFrame). Ogre skips a clean
// static map's scene pass, its six cube faces and its DPSM copy
// (_shouldUpdateShadowMapIdx), and — because our definition clears per map
// rather than per atlas — its clear too.
//
// PER INSTANCE, EVERY KIND (F8; P4/P5). The fixed-light table and the dirty
// flags live in each CompositorShadowNode, i.e. per WORKSPACE: every view, every
// planar budget slot and every shadowed reflection probe holds its own. They
// all get the same assignment, which is what makes a probe capture render a
// dirty lamp map on its FIRST face and reuse it on the other five, and the
// planar mirror reuse its lamp maps at rest. Marking is lazy by construction:
// a probe that does not capture this frame keeps the dirty flag until it does.
//
// THE SLOT RULE (planCachedSlots). Every point before every spot (HlmsPbs's
// type order); a lamp keeps the slot it has whenever that order allows, so an
// added or hidden lamp re-renders nothing but itself; otherwise the canonical
// layout, start-aligned (points by id, then spots by id).
//
// THE V1 OVER-BUDGET RULE. An instance caches only while every lamp fits its
// maps; with more lamps than maps it releases them all to Ogre's
// closest-first dynamic sort (which picks different lamps as the camera
// moves — caching some of them would freeze an arbitrary subset), and
// shadowStatus() counts it in `uncachedInstances`.

/// The shadow-pass counter, one per kind. Counts passes whose parent node IS
/// that kind's shadow node, per shadow-map index, so "a cached map rendered
/// this frame" is a measurement and not an inference. Reset explicitly at the
/// top of each frame (not in workspacePreUpdate: one counter rides many
/// workspaces — every probe's, every planar slot's).
class OgreEngine::ShadowPassCounter final : public Ogre::CompositorWorkspaceListener {
public:
    explicit ShadowPassCounter(const char *nodeName) : mNode(nodeName) {}
    void passPreExecute(Ogre::CompositorPass *pass) override {
        const Ogre::CompositorNode *node = pass->getParentNode();
        if (!node || node->getName() != mNode) return;
        // THE CASTER CUT'S PASS (ATOM-SHADOWS-1) is the second half of the scene pass it
        // follows -- the same map's one render -- so it is not a pass of its own here.
        if (pass->getType() == Ogre::PASS_CUSTOM) {
            const auto *ad = dynamic_cast<const AtomPassDef *>(pass->getDefinition());
            if (ad && ad->mCustomId == Ogre::IdString(kAtomCasterPassId)) return;
        }
        ++mTotal;
        const Ogre::uint32 idx = pass->getDefinition()->mShadowMapIdx;
        if (idx < kMaxTrackedMaps) ++mPerMap[idx];
    }
    void reset() { mTotal = 0; for (unsigned &c : mPerMap) c = 0; }
    unsigned total() const { return mTotal; }
    unsigned perMap(unsigned idx) const { return idx < kMaxTrackedMaps ? mPerMap[idx] : 0u; }
    /// Passes of the focused (point/spot) maps: every map index past the three
    /// PSSM splits.
    unsigned lamps() const {
        unsigned n = 0;
        for (unsigned i = 3u; i < kMaxTrackedMaps; ++i) n += mPerMap[i];
        return n;
    }
    static constexpr unsigned kMaxTrackedMaps = 3u + kMaxShadowMaps;

private:
    Ogre::IdString mNode;
    unsigned mTotal = 0;
    unsigned mPerMap[kMaxTrackedMaps] = { 0 };
};

namespace {
const char *shadowNodeNameOf(ShadowNodeKind k) {
    switch (k) {
    case ShadowNodeKind::View:    return OgreView::kShadowNodeName;
    case ShadowNodeKind::Reflect: return OgreView::kReflectShadowNodeName;
    case ShadowNodeKind::Probe:   return OgreView::kProbeShadowNodeName;
    }
    return OgreView::kShadowNodeName;
}

void attachOnce(Ogre::CompositorWorkspace *ws, Ogre::CompositorWorkspaceListener *l) {
    const Ogre::CompositorWorkspaceListenerVec &ls = ws->getListeners();
    if (std::find(ls.begin(), ls.end(), l) == ls.end()) ws->addListener(l);
}
}   // namespace

void OgreEngine::detachShadowCounter() {
    // The VIEW counter rides a view (through its workspace seam); the other
    // two ride the scenes' private workspaces directly. Only LIVE workspaces
    // are touched — a workspace that died took its listener list with it, so
    // no pointer to one is ever kept.
    ShadowPassCounter *viewCounter = mShadowCounters[unsigned(ShadowNodeKind::View)];
    if (mShadowCounterView && viewCounter) {
        for (const auto &v : mViews)
            if (v.get() == mShadowCounterView) v->removeWorkspaceListener(viewCounter);
    }
    mShadowCounterView = nullptr;
    for (unsigned k = 1u; k < kShadowNodeKinds; ++k) {
        if (!mShadowCounters[k]) continue;
        std::vector<Ogre::CompositorWorkspace *> ws;
        for (const auto &sc : mScenes) sc->shadowWorkspaces(ShadowNodeKind(k), ws);
        for (Ogre::CompositorWorkspace *w : ws) w->removeListener(mShadowCounters[k]);
    }
    for (ShadowPassCounter *&c : mShadowCounters) { delete c; c = nullptr; }
    // P8: a detached counter reads NOTHING — never the last number it saw.
    mShadowPassesLastFrame = 0;
    mCachedMapRendersLastFrame = 0;
    for (unsigned k = 0; k < kShadowNodeKinds; ++k) mShadowKindPasses[k] = mShadowKindLampPasses[k] = 0;
    mShadowViewMapPasses.clear();
}

void OgreEngine::noteViewDestroyed(OgreView *view) {
    // THE VIEW COUNTER RIDES A VIEW, and this is where that view can die.
    // Without this the next frame's applyShadowCache dereferences a freed
    // OgreView to detach its listener — found by ASan on test_engine_asan's
    // shadow_resolution_rebuilds_the_atlas, which destroys views while the
    // counter is attached.
    if (!view || view != mShadowCounterView) return;
    if (ShadowPassCounter *c = mShadowCounters[unsigned(ShadowNodeKind::View)])
        view->removeWorkspaceListener(c);
    mShadowCounterView = nullptr;
}

std::vector<hud::AtlasTileDesc> OgreEngine::collectAtlasTiles() const {
    std::vector<hud::AtlasTileDesc> tiles;
    if (!mHlmsRegistered || mHeadless) return tiles;
    JAH_TRY {
        // The first enabled view that actually has a shadow node — the same
        // "one view speaks for the process" rule the HUD itself follows, and
        // the reason the verb documents that a second on-screen view shows the
        // first one's tiles.
        const Ogre::CompositorShadowNode *node = nullptr;
        for (const auto &v : mViews) {
            if (!v->isEnabled()) continue;
            if (const Ogre::CompositorShadowNode *n = v->shadowNodeInstance()) { node = n; break; }
        }
        if (!node) return tiles;
        const Ogre::CompositorShadowNodeDef *def =
            static_cast<const Ogre::CompositorShadowNodeDef *>(node->getDefinition());
        if (!def) return tiles;
        OgreScene *primary = nullptr;
        {
            std::vector<OgreScene *> scenes;
            scenesFeedingEnabledViews(scenes);
            if (!scenes.empty()) primary = scenes.front();
        }
        const Ogre::LightClosestArray &lights = node->getShadowCastingLights();
        const size_t numMaps = def->getNumShadowTextureDefinitions();
        for (size_t i = 0; i < numMaps; ++i) {
            const Ogre::ShadowTextureDefinition *td = def->getShadowTextureDefinition(i);
            if (!td) continue;
            hud::AtlasTileDesc tile;
            // getDefinedTexture is how upstream's own debug overlay reaches a
            // shadow atlas (ShadowMapDebuggingGameState).
            tile.tex = const_cast<Ogre::CompositorShadowNode *>(node)->getDefinedTexture(
                td->getTextureName());
            if (!tile.tex) continue;
            tile.u0 = float(td->uvOffset.x);
            tile.v0 = float(td->uvOffset.y);
            tile.u1 = float(td->uvOffset.x + td->uvLength.x);
            tile.v1 = float(td->uvOffset.y + td->uvLength.y);
            // The caption says what the map IS, which is the whole point of the
            // overlay: which light, and whether the lamp-map cache holds its
            // content and is therefore not redrawing it.
            std::string label = "M" + std::to_string(i);
            const size_t lightIdx = td->light;
            if (lightIdx < lights.size() && lights[lightIdx].light) {
                const Ogre::Light *l = lights[lightIdx].light;
                switch (l->getType()) {
                case Ogre::Light::LT_DIRECTIONAL: label += " sun s" + std::to_string(td->split); break;
                case Ogre::Light::LT_POINT:       label += " point"; break;
                case Ogre::Light::LT_SPOTLIGHT:   label += " spot"; break;
                default:                          label += " ?"; break;
                }
                if (primary) {
                    const NodeId n = primary->nodeOfLight(l);
                    if (n) label += " #" + std::to_string((unsigned long long)n);
                }
                // "cached" = held by the lamp-map cache (ENGINE_CACHE_POLICY P2),
                // "*" = re-rendering this frame.
                if (lights[lightIdx].isStatic)
                    label += lights[lightIdx].isDirty ? " cached*" : " cached";
            } else {
                label += " empty";
            }
            tile.label = label;
            tiles.push_back(tile);
        }
    } JAH_CATCH(mLastError, tiles);
    return tiles;
}

bool OgreEngine::refreshShadows() {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { return e.refreshShadows(); })) return true;
    if (!mHlmsRegistered || mHeadless) return false;
    bool any = false;
    for (auto &s : mScenes) { s->dirtyAllShadowMaps(); any = true; }
    return any;
}

void OgreEngine::applyShadowCache() {
    if (!mHlmsRegistered || mHeadless) return;
    ++mShadowFrame;
    JAH_TRY {
        // THE CLEAR STRATEGY FOLLOWS THE NEED — ONE WAY. Per-map clear quads are
        // what let a cached map survive its atlas neighbours being redrawn;
        // without a cacheable lamp they buy nothing and cost three quads over
        // the PSSM block where a hardware fast-clear covered the whole atlas
        // (measured, gi.coalesce 2026-09-09 — see buildShadowNode). So a process
        // that has only ever drawn sun-lit scenes keeps upstream's pass list
        // exactly, and the first lamp rebuilds the three node definitions once:
        // the same hitch a Shadow Quality change costs.
        //
        // It never flips BACK within the session (the map count's D4 rule).
        // A rebuild drops every workspace that names a shadow node — the
        // views', the mirrors' and the reflection probes', whose GI arm is
        // rebuilt from scratch — so a two-way switch would turn a script
        // toggling a scene's only lamp into a multi-second hitch per toggle.
        // The quads over an idle atlas cost next to nothing by comparison.
        //
        // Only a scene that HAS a shadow-node instance counts: a preview drawn
        // with shadows off (the Materials page's sphere, lit by a key and a
        // shadow-casting fill) has nothing to cache, and flipping the whole
        // process's atlas for it was a needless rebuild of every shadowed arm.
        //
        // AND IT WAITS FOR THE WORLD TO BE UP (the sun lane, 2026-09-13). This
        // flip used to fire on the very first frame a cacheable lamp was seen,
        // which — the moment shadows became the default for every new light —
        // meant it fired during the SCENE'S FIRST BIND, in the same frames
        // where the post chain is being built and the texture streamer is
        // uploading into its pooled Type2DArrays. Dropping and recreating every
        // workspace that names a shadow node right there raced the streamer:
        // 3-10 `VUID-vkCmdDraw-None-09600` image-layout errors per boot (a
        // colour array expected in TRANSFER_DST found SHADER_READ_ONLY), which
        // app.engine_selftest_validation is the only suite that can see. The
        // pixels were never wrong — the hazard is, and it was there for any
        // scene that opened with a casting lamp, which is four of the eight we
        // ship.
        //
        // So the flip is debounced exactly as the atlas GROWTH beside it
        // already is (that one carries the same comment: "a scene loads its
        // lights over many frames and every rebuild drops and recreates every
        // workspace"), with one addition — the frames only count while the
        // views that want it are PRESENTING. A world that is still binding
        // never advances the counter, so the rebuild lands in a quiet frame
        // after the first presents instead of inside them. It costs at most
        // kShadowClearFlipDebounceFrames presented frames of the old clear
        // strategy, during which nothing is cached yet anyway.
        if (!mShadowPerMapClears) {
            // SCRATCH, not locals: in a scene whose only light is the sun this
            // branch runs on every frame for the life of the process (the flip
            // it debounces never happens), and it built two vectors each time
            // (clean-2 lane, 2026-09-13).
            std::vector<OgreScene *> &scenes = mShadowSceneScratch;
            scenes.clear();
            scenesFeedingEnabledViews(scenes);
            bool anyLamp = false;
            bool presenting = false;
            for (OgreScene *s : scenes) {
                if (!s->hasCacheableShadowLights()) continue;
                bool shadowed = false;
                bool shown = false;
                for (auto &v : mViews) {
                    if (v->ogreScene() != s || !v->shadowNodeInstance()) continue;
                    shadowed = true;
                    if (v->framesPresented() > 0u) shown = true;
                }
                std::vector<Ogre::CompositorWorkspace *> &ws = mShadowWsScratch;
                ws.clear();
                s->shadowWorkspaces(ShadowNodeKind::Reflect, ws);
                s->shadowWorkspaces(ShadowNodeKind::Probe, ws);
                if (shadowed || !ws.empty()) {
                    anyLamp = true;
                    // A scene whose only shadow-node instances are a mirror's
                    // or a probe's has no view counter to wait on; it is not
                    // in a first bind either, since those arms are built after
                    // the view is up.
                    if (shown || !shadowed) presenting = true;
                    break;
                }
            }
            // WHAT THE DERIVATION NEEDS TO KNOW (mShadowClearFlipWanted): the
            // flip is coming, it is only waiting out its debounce. If the
            // derivation has to rebuild the atlas for a bigger count in the
            // meantime, it carries the flip in the same rebuild and this
            // debounce never fires at all.
            mShadowClearFlipWanted = anyLamp && presenting;
            if (!anyLamp || !presenting) {
                mShadowClearFlipFrames = 0;
            } else if (++mShadowClearFlipFrames >= kShadowClearFlipDebounceFrames) {
                mShadowClearFlipFrames = 0;
                // ONE REBUILD, NOT TWO (E2 review, ledger 104/121 — the
                // "first-lamp atlas hitch"). This used to rebuild at the count
                // the atlas HAPPENED to have, and deriveShadowMapCount then
                // rebuilt AGAIN, three frames later, when it noticed that the
                // same lamps wanted a bigger one. A rebuild drops and recreates
                // every workspace that names a shadow node — the views', the
                // mirrors', and every reflection probe's, whose GI arm is built
                // from scratch — so a world opening with three or more casting
                // lamps paid that twice, in two separate frames, for one event.
                // The flip now carries the demand the derivation is about to
                // ask for; the derivation then finds `want <= mShadowMapCount`
                // and does nothing. Never SHRINKS here either (owner decision
                // D4): the demand is floored at what the atlas already has.
                unsigned casters = 0;
                const unsigned want = std::max(mShadowMapCount, shadowMapDemand(&casters));
                // SAID OUT LOUD ON THIS PATH TOO (F3, round 2). shadowMapDemand
                // carries the over-budget warning's bookkeeping with it, and a
                // growth that happens here gets the same log line the derived
                // path writes — a rebuild this expensive may not be silent
                // just because a different branch triggered it.
                if (want > mShadowMapCount) logShadowAtlasGrowth(want, casters, "clear-strategy flip");
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka shadows: the atlas clears per map from now on (a cacheable "
                    "point/spot lamp is drawn); one rebuild of every shadow-node workspace");
                rebuildShadowAtlas(mShadowResolution, want, true);
                // ...and the derivation's own debounce is reset with it, so a
                // count it was part-way through counting up to is not applied
                // a second time.
                mDerivedShadowMapWant = 0;
                mDerivedShadowMapFrames = 0;
            }
        }

        // THE SUBSCRIPTION ENDED (U3 stage 3; P8's expiry): the per-pass callbacks come off the render path and the
        // readings go to 0 — the counters run for exactly the subscribed span.
        if (!shadowLeased()) {
            mShadowLeaseUntil = 0;
            mShadowCountersArmed = false;
            if (mShadowCounters[0] || mShadowCounters[1] || mShadowCounters[2]) detachShadowCounter();
        }
    } JAH_CATCH(mLastError, );
}

void OgreEngine::releaseShadowLamp(OgreScene *scene, Ogre::Light *light) {
    // A light is about to be destroyed: untie it from every instance that
    // holds it FIXED, now — not on the next frame. Ogre dereferences a fixed
    // light on every update of the node (clearShadowCastingLights flips its
    // cast-shadows flag), and an instance that does not update this frame (a
    // probe that does not capture, a disabled view) would carry the dangling
    // pointer until it did; worse, a new light at the recycled address would
    // read as "already cached" with the dead lamp's map.
    if (!scene || !light || !mHlmsRegistered || mHeadless) return;
    JAH_TRY {
        std::vector<Ogre::CompositorShadowNode *> nodes;
        for (auto &v : mViews)
            if (v->ogreScene() == scene)
                if (Ogre::CompositorShadowNode *n = v->shadowNodeInstance()) nodes.push_back(n);
        for (unsigned k = 1u; k < kShadowNodeKinds; ++k) {
            std::vector<Ogre::CompositorWorkspace *> ws;
            scene->shadowWorkspaces(ShadowNodeKind(k), ws);
            for (Ogre::CompositorWorkspace *w : ws)
                if (Ogre::CompositorShadowNode *n = w->findShadowNode(shadowNodeNameOf(ShadowNodeKind(k))))
                    nodes.push_back(n);
        }
        for (Ogre::CompositorShadowNode *n : nodes) {
            const Ogre::LightClosestArray &held = n->getShadowCastingLights();
            for (size_t slot = 1; slot < held.size(); ++slot)
                if (held[slot].isStatic && held[slot].light == light) {
                    n->setLightFixedToShadowMap(slot + 2u, nullptr);
                    // A RELEASE IS AN ASSIGNMENT CHANGE, and this is the SECOND
                    // place one happens (clean-2 lane review, F3): the cache's
                    // pass-hash self-check only runs on nodes marked here and in
                    // applyShadowCacheDirties, so without this the frame after a
                    // lamp was destroyed went unchecked — exactly the frame in
                    // which the node's slot array and its cached light count are
                    // most likely to disagree.
                    detail::FogHlmsListener::noteShadowAssignmentChanged(n);
                }
        }
    } JAH_CATCH(mLastError, );
}

namespace {
/// WHICH LAMP IN WHICH SLOT (focused slots 1..maps, returned 0-based). The
/// assignment is STABLE: a re-fixed lamp is a re-rendered map, so a lamp that
/// already holds a slot keeps it whenever HlmsPbs's order still holds — every
/// point before every spot (the pass buffer is read as cumulative type ranges;
/// empty slots in between are skipped) — and a new lamp takes a free slot
/// inside its own type's region. Only when that is impossible (a new point
/// with no free slot before the first spot, a type change breaking the order)
/// does it fall back to the canonical layout, START-aligned: points by id,
/// then spots by id — which shifts only the slots after the change. In cache
/// mode no dynamic lamp shares the node, so nothing needs the old end-of-range
/// placement (F7 kept statics clear of Ogre's closest-first sort; there is no
/// sort left to keep clear of). Adding a lamp therefore renders that lamp's
/// map alone in the common case, and hiding one re-fixes nothing.
// (`plan` and `placed` are the caller's scratch — this runs per instance per
// frame for ever, so neither is built here; clean-2 lane, 2026-09-13.)
void planCachedSlots(const Ogre::LightClosestArray &held, size_t maps,
                     const std::vector<Ogre::Light *> &want, size_t fixed,
                     std::vector<Ogre::Light *> &plan, std::vector<char> &placed) {
    plan.assign(maps, nullptr);
    if (!fixed) return;
    const auto isSpot = [](const Ogre::Light *l) { return l->getType() == Ogre::Light::LT_SPOTLIGHT; };
    // 1. Keep every lamp that is still wanted where it is.
    placed.assign(want.size(), 0);
    for (size_t j = 0; j < maps; ++j) {
        const Ogre::LightClosest &e = held[j + 1u];
        if (!e.isStatic || !e.light) continue;
        const auto it = std::find(want.begin(), want.end(), e.light);
        if (it == want.end()) continue;
        plan[j] = e.light;
        placed[size_t(it - want.begin())] = 1;
    }
    const auto regions = [&](size_t &lastPoint, size_t &firstSpot) {
        lastPoint = 0; firstSpot = maps;
        bool anyPoint = false;
        for (size_t j = 0; j < maps; ++j) {
            if (!plan[j]) continue;
            if (isSpot(plan[j])) firstSpot = std::min(firstSpot, j);
            else { lastPoint = j; anyPoint = true; }
        }
        return anyPoint;
    };
    size_t lastPoint = 0, firstSpot = maps;
    bool anyPoint = regions(lastPoint, firstSpot);
    bool ok = !anyPoint || lastPoint < firstSpot;
    // 2. Place the new lamps in `want` order (points by id, then spots by id).
    for (size_t i = 0; ok && i < want.size(); ++i) {
        if (placed[i]) continue;
        size_t j = maps;
        if (!isSpot(want[i])) {
            for (size_t c = 0; c < firstSpot && c < maps; ++c) if (!plan[c]) { j = c; break; }
            if (j == maps && firstSpot < maps) {
                // THE POINTS' REGION IS FULL AND A SPOT SITS AT ITS EDGE. Moving
                // that ONE spot to a free slot keeps the type order and costs a
                // single re-rendered map — where re-laying the whole range out
                // (step 3) would re-render every spot. Any free slot is above
                // the last point, because everything below firstSpot is taken.
                for (size_t c = firstSpot; c < maps; ++c) if (!plan[c]) { j = c; break; }
                if (j != maps) {
                    plan[j] = plan[firstSpot];      // the evicted spot, one slot up
                    j = firstSpot;
                }
            }
        } else {
            for (size_t c = anyPoint ? lastPoint + 1u : 0u; c < maps; ++c) if (!plan[c]) { j = c; break; }
        }
        if (j == maps) { ok = false; break; }
        plan[j] = want[i];
        placed[i] = 1;
        anyPoint = regions(lastPoint, firstSpot);
    }
    if (ok) return;
    // 3. The canonical layout.
    std::fill(plan.begin(), plan.end(), nullptr);
    for (size_t i = 0; i < fixed && i < maps; ++i) plan[i] = want[i];
}
/// THE NEAREST `maps` LAMPS, IN SLOT ORDER (E2 review finding, ledger 104/121).
///
/// The cache used to be all-or-nothing: `want.size() <= maps` or no caching at
/// all. On the PROBE node that rule is a cliff, because its focused count is
/// capped at four whatever the main atlas grew to
/// (kProbeShadowMaxFocusedMaps — 32 probes x an R/4 atlas is the VRAM this cap
/// exists to hold). A room with FIVE lamps therefore took every probe instance
/// in the scene out of the cache at once: 32 instances re-rendering four lamp
/// maps each, every capture, for ever, over one lamp too many.
///
/// A probe's camera never moves (it sits at the probe centre), so the four
/// lamps nearest to it are a STABLE choice — which is exactly what the cache
/// needs, and exactly what Ogre would have picked per frame anyway
/// (OgreCompositorShadowNode.cpp:403-430 keeps the closest casters). Fixing
/// them is the same picture at none of the cost.
///
/// TWO INVARIANTS ARE LOAD-BEARING HERE.
///   * SLOT ORDER: the survivors keep their position in `want` (every point by
///     id, then every spot by id). HlmsPbs reads the shadow-casting list as
///     cumulative type ranges, so a spot promoted above a point would be shaded
///     as a point (ShadowCacheFrame::lights' note).
///   * STICKINESS: a lamp this instance already holds counts as 10% nearer
///     (kShadowNearestStickiness on the SQUARED distance), so a lamp drifting
///     across the boundary re-renders one map once instead of flapping two maps
///     every frame.
/// Ties break on the index, so the choice is deterministic frame to frame.
void keepNearestLamps(std::vector<Ogre::Light *> &want, size_t maps, const Ogre::Vector3 &eye,
                      const Ogre::LightClosestArray &held,
                      std::vector<std::pair<float, size_t>> &order)
{
    if (want.size() <= maps || maps == 0u) return;
    order.clear();
    order.reserve(want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        const Ogre::Node *n = want[i]->getParentNode();
        float d = n ? float(eye.squaredDistance(n->_getDerivedPosition()))
                    : std::numeric_limits<float>::max();
        for (size_t j = 1; j < held.size(); ++j)
            if (held[j].isStatic && held[j].light == want[i]) { d *= kShadowNearestStickiness; break; }
        order.emplace_back(d, i);
    }
    std::sort(order.begin(), order.end());          // (distance, index)
    order.resize(maps);
    std::sort(order.begin(), order.end(),
              [](const std::pair<float, size_t> &a, const std::pair<float, size_t> &b) {
                  return a.second < b.second;       // back into slot order
              });
    for (size_t i = 0; i < maps; ++i) want[i] = want[order[i].second];   // i <= order[i].second
    want.resize(maps);
}

}   // namespace

void OgreEngine::applyShadowCacheDirties(const std::vector<OgreScene *> &drawn) {
    for (unsigned k = 0; k < kShadowNodeKinds; ++k)
        mShadowCachedInstances[k] = mShadowUncachedInstances[k] = mShadowDirtiedMaps[k] = 0;
    // THE SELF-CHECK'S WINDOW CLOSES HERE, not at the end of this function: the
    // marks made below must survive the frame's passes (that is when the pass
    // hashes are built), so they are dropped at the head of the NEXT frame.
    // See FogHlmsListener::noteShadowAssignmentChanged.
    detail::FogHlmsListener::clearShadowAssignmentChanges();
    if (!mHlmsRegistered || mHeadless) return;
    JAH_TRY {
        // ---- the counters, armed for THIS frame (P8) ----------------------
        // Here and not at the top of the frame: applyPendingGi / applyPendingPlanar
        // may have rebuilt a probe or planar workspace since, and the counters
        // must ride the workspaces that are about to render.
        // The frame about to render is MEASURED exactly when this arms it. A
        // reader that asks before any such frame gets countersMeasured = false
        // and reports "not measured" instead of a zero (SKY-SMALL).
        mShadowCountersArmed = shadowLeased();
        if (mShadowCountersArmed) {
            for (unsigned k = 0; k < kShadowNodeKinds; ++k) {
                if (!mShadowCounters[k])
                    mShadowCounters[k] = new ShadowPassCounter(shadowNodeNameOf(ShadowNodeKind(k)));
                mShadowCounters[k]->reset();
            }
            // The VIEW counter rides the first enabled view that has a shadow
            // node — the "one view speaks for the process" rule the HUD and the
            // post chain's recompile globals use — through the view's workspace
            // seam, so an atlas rebuild re-attaches it by itself.
            OgreView *counterView = nullptr;
            for (auto &v : mViews)
                if (v->isEnabled() && v->ogreScene() && v->shadowNodeInstance()) { counterView = v.get(); break; }
            ShadowPassCounter *viewCounter = mShadowCounters[unsigned(ShadowNodeKind::View)];
            if (counterView != mShadowCounterView) {
                // Only ever detach from a view still in mViews: a raw view
                // pointer held across frames is the shape of the use-after-free
                // ASan caught here once.
                for (auto &v : mViews)
                    if (v.get() == mShadowCounterView) v->removeWorkspaceListener(viewCounter);
                mShadowCounterView = counterView;
            }
            if (mShadowCounterView) mShadowCounterView->addWorkspaceListener(viewCounter);
            for (OgreScene *s : drawn)
                for (unsigned k = 1u; k < kShadowNodeKinds; ++k) {
                    std::vector<Ogre::CompositorWorkspace *> &ws = mShadowWsScratch;
                    ws.clear();
                    s->shadowWorkspaces(ShadowNodeKind(k), ws);
                    for (Ogre::CompositorWorkspace *w : ws) attachOnce(w, mShadowCounters[k]);
                }
        }

        // ---- detection per scene, application per instance ----------------
        for (OgreScene *s : drawn) {
            std::vector<ShadowInstance> &instances = mShadowInstScratch;
            instances.clear();
            // EVERY view of the scene, enabled or not: a disabled view keeps its
            // workspace, and marking its instance now is what keeps its maps
            // honest for the frame it is shown again (the flags persist).
            for (auto &v : mViews)
                if (v->ogreScene() == s)
                    if (Ogre::CompositorShadowNode *n = v->shadowNodeInstance()) {
                        Ogre::Camera *c = v->camera();
                        instances.push_back({ n, ShadowNodeKind::View,
                                              c ? c->getDerivedPosition() : Ogre::Vector3::ZERO,
                                              c != nullptr });
                    }
            for (unsigned k = 1u; k < kShadowNodeKinds; ++k) {
                std::vector<Ogre::CompositorWorkspace *> &ws = mShadowWsScratch;
                ws.clear();
                s->shadowWorkspaces(ShadowNodeKind(k), ws);
                for (Ogre::CompositorWorkspace *w : ws)
                    if (Ogre::CompositorShadowNode *n = w->findShadowNode(shadowNodeNameOf(ShadowNodeKind(k)))) {
                        // The workspace's own camera IS the eye this instance's
                        // maps will be chosen for: a mirror slot's reflected
                        // camera, and — the case this matters for — a probe's
                        // capture camera, which CubemapProbe parks at the probe
                        // centre and never moves (OgreCubemapProbe.cpp:562).
                        Ogre::Camera *c = w->getDefaultCamera();
                        instances.push_back({ n, ShadowNodeKind(k),
                                              c ? c->getDerivedPosition() : Ogre::Vector3::ZERO,
                                              c != nullptr });
                    }
            }
            // THE FRAME'S ONE ITEM WALK: the GI movement records and — when
            // this scene has shadow nodes to cache into and lamps to cache — the
            // caster changes, in one pass on the bounds updateSceneGraph just
            // made current. A preview drawn with shadows off gets only the GI
            // half (if any); a node that appears later is new, and a new
            // assignment renders its maps regardless.
            s->runItemWalk(!instances.empty() && s->hasCacheableShadowLights());
            if (instances.empty()) continue;
            OgreScene::ShadowCacheFrame &f = mShadowFrameScratch;
            s->collectShadowCacheFrame(f);
            for (const ShadowInstance &in : instances) {
                const unsigned k = unsigned(in.kind);
                const Ogre::LightClosestArray &held = in.node->getShadowCastingLights();
                if (held.size() < 2u) continue;                  // a PSSM-only node
                const size_t maps = held.size() - 1u;            // light slots 1..maps
                std::vector<Ogre::Light *> &want = mShadowWantScratch;
                want.clear();
                want.reserve(f.lights.size());
                for (const OgreScene::ShadowCacheLight &l : f.lights) want.push_back(l.light);
                // A PROBE WITH MORE LAMPS THAN MAPS CACHES ITS NEAREST FOUR
                // rather than caching nothing (keepNearestLamps' note). Only the
                // probe kind: its camera is parked at the probe centre, so the
                // choice is stable and re-rendering it costs nothing to churn.
                // A VIEW and a MIRROR keep the v1 rule (ENGINE_CACHE_POLICY_SPEC
                // P2, tests/shadow T4) — their cameras follow the user, Ogre's
                // per-frame closest-first choice IS the right picture there, and
                // `viewCached` false / `uncachedInstances` is how the status
                // says so.
                if (in.kind == ShadowNodeKind::Probe && in.hasEye)
                    keepNearestLamps(want, maps, in.eye, held, mShadowNearScratch);
                // Cache only with per-map clears (a whole-atlas clear would wipe
                // a cached map every frame) and only while every lamp fits.
                const bool cache = mShadowPerMapClears && !want.empty() && want.size() <= maps;
                if (cache) ++mShadowCachedInstances[k];
                else if (!want.empty()) ++mShadowUncachedInstances[k];
                const size_t fixed = cache ? want.size() : 0u;
                const std::vector<Ogre::Light *> &dirty = f.dirty[k];
                static const char *kKindNames[kShadowNodeKinds] = { "view", "reflect", "probe" };
                // THE MONITOR'S REASON LOOKUP (§4.7): why this lamp's map is
                // dirty, straight from the detection pass that decided it.
                const auto reasonFor = [&](const Ogre::Light *l) -> WorkReason {
                    if (f.dirtyAll) return WorkReason::Request;
                    for (size_t d = 0; d < dirty.size(); ++d)
                        if (dirty[d] == l)
                            return d < f.dirtyReason[k].size() ? f.dirtyReason[k][d]
                                                              : WorkReason::None;
                    return WorkReason::None;
                };
                // NO LAMP MAY SIT IN TWO SLOTS. A slot Ogre's dynamic sort filled
                // on this instance's last update still names its light until the
                // next update rebuilds it — and fixing that same light into
                // another slot now would list it twice. Upstream's Forward+
                // hides every listed light around its cull and restores each
                // slot's RECORDED visibility (OgreForwardClustered.cpp:925-951):
                // the second occurrence records "hidden", so the light ends the
                // frame hidden for good. Measured: a GI rebuild's synchronous
                // probe placement left such entries on every probe instance and
                // switched every lamp of the scene off. Clearing the stale
                // dynamic entry is free — the next update refills the dynamic
                // slots from lamps the cache does not hold.
                if (cache)
                    for (size_t j = 0; j < maps; ++j) {
                        const Ogre::LightClosest &e = held[j + 1u];
                        if (e.light && !e.isStatic &&
                            std::find(want.begin(), want.end(), e.light) != want.end()) {
                            in.node->setLightFixedToShadowMap(j + 3u, nullptr);
                            detail::FogHlmsListener::noteShadowAssignmentChanged(in.node);
                        }
                    }
                std::vector<Ogre::Light *> &plan = mShadowPlanScratch;
                planCachedSlots(held, maps, want, fixed, plan, mShadowPlacedScratch);
                for (size_t j = 0; j < maps; ++j) {
                    const size_t slot = j + 1u;
                    const size_t mapIdx = slot + 2u;             // 3 PSSM maps first
                    Ogre::Light *w = plan[j];
                    Ogre::Light *have = held[slot].isStatic ? held[slot].light : nullptr;
                    if (w != have) {
                        // A NEW assignment (or a release). setLightFixedToShadowMap
                        // marks the map dirty itself, which is why this runs only
                        // when the assignment CHANGED: every frame would keep it
                        // permanently dirty and cache nothing.
                        in.node->setLightFixedToShadowMap(mapIdx, w);
                        // AN ASSIGNMENT CHANGES HERE (and on a lamp's
                        // release, in releaseShadowLamp): this frame's passes on
                        // this node get the cache's self-check.
                        detail::FogHlmsListener::noteShadowAssignmentChanged(in.node);
                        if (w) {
                            ++mShadowDirtiedMaps[k];
                            if (monitor::live()) noteShadowMapWork(in.kind, WorkReason::Added,
                                                                   s->nodeOfLight(w), kKindNames[k]);
                        }
                    } else if (w && (f.dirtyAll ||
                                     std::find(dirty.begin(), dirty.end(), w) != dirty.end())) {
                        // includeLinked=false: our maps clear individually, so one
                        // dirty map does not oblige its atlas neighbours to redraw.
                        in.node->setStaticShadowMapDirty(mapIdx, false);
                        ++mShadowDirtiedMaps[k];
                        if (monitor::live()) noteShadowMapWork(in.kind, reasonFor(w),
                                                               s->nodeOfLight(w), kKindNames[k]);
                    }
                }
            }
            flushShadowProbeMarks();
        }
        // THE RAY-QUERY TIER'S FRAME (PHOTON_SPEC §7 R1) — here, and here only.
        // This is the frame's one point where the scene graph is current
        // (updateSceneGraph ran), the item walk has just been done, and nothing
        // has rendered yet; the acceleration-structure builds are recorded into
        // the SAME command buffer, outside every encoder, with no submit and no
        // stall. A scene that did not move records nothing (the tier gates on
        // the same transform epoch the caster walk does).
        //
        // IT IS A MEASURED STAGE. The monitor's stages are exclusive and must
        // account for the frame within 5% (`monitor_passes_sum_to_the_frame`),
        // and this work is real CPU inside renderOneFrame that belongs to no
        // compositor pass — unattributed, it read as frame time nobody spent.
        // THE GPU SCENE'S SUBSTRATE, immediately before its first reader in the
        // frame and for the same reason the ray tier runs here: this is the frame's
        // one point where the scene graph is current and nothing has rendered. The
        // frame's read is a COMPOSE (the frame's walk — the dirty scan, a
        // UniformScalableTask on the scene's workers — unless a GI build before
        // the frame already ran it, and every mark since), the RAY LEVELS patching
        // the mirror, then ONE COMMIT into the frame's copy of the instance table
        // (GpuSceneSubstrate.h). The commit is recorded before any workspace pass:
        // the frame's first GPU work after the graph. Its monitor stage is
        // `engine.gpuscene` (the compose and the commit).
        // THE PBS CHANGE LOG FIRST (ATOM-CPU-WALKS-1): the Items an in-place
        // material edit re-hashed are marked here, so the scan below re-composes
        // them — route, queue, word, flags — in this frame.
        // THE CULL TWINS NOTHING WEARS ANY MORE (CULL-MODE-2), before the drain and
        // every reader of an Item's datablock: a flag test for a scene with none.
        for (OgreScene *s : drawn) s->sweepCullTwins();
        drainPbsChanges();
        for (OgreScene *s : drawn) s->composeGpuSceneFrame();
        // THE FRAME'S ONE COMMIT: the composed slots in one staging map, into the frame's
        // copy — before the ray tier reads it. (The ray level each slot's near copy is
        // built from is level 0, fixed: PHOTON-7 stage 6 deleted the per-view rule.)
        for (OgreScene *s : drawn) s->commitGpuSceneFrame();
        // THE CUT-OUTS' MASKS (ENGINE-POLISH-ATOM-1 stage 3): every drawn scene's table after its commit (the flags
        // word it reads is this frame's), the masks it asks for made on the device, then ONE edge for every address
        // reader - before the ray tier (which reads the table) and every pass.
        for (OgreScene *s : drawn) s->updateMaskTable();
        alphaMaskStore().endFrame(mRoot ? mRoot->getRenderSystem() : nullptr);
        {
            const auto rqStart = std::chrono::steady_clock::now();
            updateRayQuery(drawn);
            if (monitor::live())
                monitor::gMonitor->stage("engine.rayquery",
                                         std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - rqStart).count());
        }
        // THE MOVERS INTO THE CHANGED-REGION LIST (PHOTON-7 stage 3): the commit's moved slots and the skin pass's posed
        // ones, here where both are this frame's (the list closes before the submit).
        listRayMovers(drawn);
        // THE VISIBILITY BUFFER'S FRAME HALF (ATOM S3-DRAW), ONCE: the screen
        // decode's draws follow the words the split's items wear, here, outside the
        // compositor, after the table the split writes into and after the ray tier
        // (whose hit sync makes the twins its own words need — it never forgets one:
        // a twin leaves only at the frame's PBS drain, above, or at a material
        // seam between frames). ITS OWN MONITOR STAGE (W13: the words walk + sort on
        // a mover).
        {
            monitor::Stage wordsStage("engine.atomwords");
            for (OgreScene *s : drawn) s->updateAtomDraw();
        }
    } JAH_CATCH(mLastError, );
}

// THE MONITOR'S SHADOW-MAP RECORDS (§4.7), with the PROBE kind coalesced.
//
// A view has one shadow-node instance and a planar mirror one per slot, so
// their records are already one per map render. A shadowed probe GRID has one
// instance PER PROBE — 32 in the Grand Showroom — and the cache marks a moving
// lamp's map dirty on every one of them, so one lamp wrote 35 identical records
// into the frame and 32 of those instances render nothing this frame (the
// budget captures one probe). The mark is the same fact 32 times; it is
// reported once, with the instance count in `units`, so a capture reads
// "1 light, 3 real map renders" instead of 35 lines. What is marked dirty does
// not change.
void OgreEngine::noteShadowMapWork(ShadowNodeKind kind, WorkReason reason,
                                   unsigned long long node, const char *kindName) {
    if (kind != ShadowNodeKind::Probe) {
        monitor::noteCacheWork(CacheKind::ShadowMap, reason, node, kindName, 1u);
        return;
    }
    for (ProbeMark &m : mShadowProbeMarks)
        if (m.node == node && m.reason == reason) { ++m.instances; return; }
    mShadowProbeMarks.push_back({ node, reason, 1u });
}

void OgreEngine::flushShadowProbeMarks() {
    for (const ProbeMark &m : mShadowProbeMarks)
        monitor::noteCacheWork(CacheKind::ShadowMap, m.reason, m.node, "probe", m.instances);
    mShadowProbeMarks.clear();
}

void OgreEngine::latchShadowCounters() {
    ShadowPassCounter *vc = mShadowCounters[unsigned(ShadowNodeKind::View)];
    if (!vc) {
        mShadowPassesLastFrame = 0;
        mCachedMapRendersLastFrame = 0;
        for (unsigned k = 0; k < kShadowNodeKinds; ++k) mShadowKindPasses[k] = mShadowKindLampPasses[k] = 0;
        mShadowViewMapPasses.clear();
        return;
    }
    JAH_TRY {
        // The view kind reports the COUNTED view's node — and nothing at all
        // when no enabled view has one (shadows switched off): P8's "reset on
        // detach", which used to keep reading the last frame it had counted.
        Ogre::CompositorShadowNode *node = mShadowCounterView ? mShadowCounterView->shadowNodeInstance()
                                                              : nullptr;
        mShadowViewMapPasses.assign(ShadowPassCounter::kMaxTrackedMaps, 0u);
        if (node) {
            for (unsigned i = 0; i < ShadowPassCounter::kMaxTrackedMaps; ++i)
                mShadowViewMapPasses[i] = vc->perMap(i);
            mShadowPassesLastFrame = vc->total();
            unsigned cachedRenders = 0;
            const Ogre::LightClosestArray &held = node->getShadowCastingLights();
            for (size_t slot = 1; slot < held.size(); ++slot)
                if (held[slot].isStatic) cachedRenders += vc->perMap(unsigned(slot + 2u));
            mCachedMapRendersLastFrame = cachedRenders;
        } else {
            mShadowPassesLastFrame = 0;
            mCachedMapRendersLastFrame = 0;
        }
        mShadowKindPasses[0] = mShadowPassesLastFrame;
        mShadowKindLampPasses[0] = node ? vc->lamps() : 0u;
        for (unsigned k = 1u; k < kShadowNodeKinds; ++k) {
            mShadowKindPasses[k] = mShadowCounters[k] ? mShadowCounters[k]->total() : 0u;
            mShadowKindLampPasses[k] = mShadowCounters[k] ? mShadowCounters[k]->lamps() : 0u;
        }
    } JAH_CATCH(mLastError, );
}

// ---------------------------------------------------------------------------
// THE SUN MAPS' CONTENT (Scene::shadowMapDigests; ENGINE-UPGRADE-U5 stage 4's parity door): the
// first view's atlas read back synchronously (Image2 - a test's read, never a frame's) and each
// active directional map's rectangle hashed texel for texel.
bool OgreScene::shadowMapDigests(std::vector<ShadowMapDigest> &out)
{
    // A READ OF THE ENGINE'S TEXTURES: on the host's side a REQUEST, answered on the engine's side (U3 stage
    // 4a; decision 16.1 - it read the atlas from the host thread).
    if (hostPosts()) { bool r = false; requestOnEngine([&] { r = shadowMapDigests(out); }); return r; }
    JAH_HOST_CALL("Scene::shadowMapDigests");
    out.clear();
    JAH_TRY {
        // THE VIEW'S NODE first, then every mirror's (kind 1: a mirror's sun maps ride the same cull).
        std::vector<std::pair<Ogre::CompositorShadowNode *, unsigned>> nodes;
        for (OgreView *v : mEngine ? mEngine->viewsOf(this) : std::vector<OgreView *>()) {
            Ogre::CompositorWorkspace *ws = v ? v->workspace() : nullptr;
            Ogre::CompositorShadowNode *sn =
                ws ? ws->findShadowNode(Ogre::IdString(OgreView::kShadowNodeName)) : nullptr;
            if (sn && sn->getDefinedTexture(Ogre::IdString("atlas0"))) { nodes.push_back({ sn, 0u }); break; }
        }
        if (nodes.empty()) return false;
        {
            std::vector<Ogre::CompositorWorkspace *> mirrors;
            shadowWorkspaces(ShadowNodeKind::Reflect, mirrors);
            for (Ogre::CompositorWorkspace *w : mirrors)
                if (Ogre::CompositorShadowNode *n = w ? w->findShadowNode(Ogre::IdString(OgreView::kReflectShadowNodeName)) : nullptr)
                    nodes.push_back({ n, 1u });
        }
        for (const auto &nk : nodes) {
            Ogre::CompositorShadowNode *sn = nk.first;
            Ogre::TextureGpu *atlas = sn->getDefinedTexture(Ogre::IdString("atlas0"));
            if (!atlas) continue;
            Ogre::Image2 img;
            img.convertFromTexture(atlas, 0u, 0u);
            const Ogre::TextureBox box = img.getData(0u);
            const unsigned tw = atlas->getWidth(), th = atlas->getHeight();
            const bool reverse = mRoot && mRoot->getRenderSystem() && mRoot->getRenderSystem()->isReverseDepth();
            const float clearDepth = reverse ? 0.0f : 1.0f;
            uint32_t clearBits = 0u;
            std::memcpy(&clearBits, &clearDepth, sizeof(clearBits));
            const Ogre::CompositorShadowNodeDef *def = sn->getDefinition();
            const size_t n = def->getNumShadowTextureDefinitions();
            for (size_t i = 0; i < n; ++i) {
                if (!sn->isShadowMapIdxActive(i)) continue;
                const Ogre::Light *l = sn->getLightAssociatedWith(i);
                if (!l || l->getType() != Ogre::Light::LT_DIRECTIONAL) continue;
                const Ogre::ShadowTextureDefinition *td = def->getShadowTextureDefinition(i);
                ShadowMapDigest d;
                d.kind = nk.second;
                d.map = unsigned(i);
                d.x = unsigned(td->uvOffset.x * Ogre::Real(tw) + Ogre::Real(0.5));
                d.y = unsigned(td->uvOffset.y * Ogre::Real(th) + Ogre::Real(0.5));
                d.width = unsigned(td->uvLength.x * Ogre::Real(tw) + Ogre::Real(0.5));
                d.height = unsigned(td->uvLength.y * Ogre::Real(th) + Ogre::Real(0.5));
                unsigned long long h = 1469598103934665603ull;
                for (unsigned r = d.y; r < d.y + d.height && r < th; ++r) {
                    const auto *row = static_cast<const uint32_t *>(box.at(0, r, 0));
                    for (unsigned c = d.x; c < d.x + d.width && c < tw; ++c) {
                        const uint32_t bits = row[c];
                        if (bits != clearBits) ++d.covered;
                        // FNV-1a over the texel's WORD (a fourth of the byte-wise walk's steps: the digests are only
                        // ever compared with each other, and the byte walk made a test's 50 digests cost seconds)
                        h ^= bits;
                        h *= 1099511628211ull;
                    }
                }
                d.digest = h;
                out.push_back(d);
            }
        }
        return true;
    } JAH_CATCH(mError, false);
}

// THE SUN MAPS' CASTERS BY ROUTE (Scene::shadowCasterRoutes; decision 15.1's measurement): every shown
// shadow-casting item whose world box reaches a sun map's light frustum (its view-projection onto the map's own
// rectangle of the atlas, the depth unbounded: the maps draw with depth clamp), by its route, with its level-0
// triangles.
bool OgreScene::shadowCasterRoutes(std::vector<ShadowRouteShare> &out)
{
    if (hostPosts()) { bool r = false; requestOnEngine([&] { r = shadowCasterRoutes(out); }); return r; }
    JAH_HOST_CALL("Scene::shadowCasterRoutes");
    out.clear();
    JAH_TRY {
        for (OgreView *v : mEngine ? mEngine->viewsOf(this) : std::vector<OgreView *>()) {
            Ogre::CompositorWorkspace *ws = v ? v->workspace() : nullptr;
            Ogre::CompositorShadowNode *sn =
                ws ? ws->findShadowNode(Ogre::IdString(OgreView::kShadowNodeName)) : nullptr;
            if (!sn) continue;
            const size_t n = sn->getDefinition()->getNumShadowTextureDefinitions();
            for (size_t i = 0; i < n; ++i) {
                if (!sn->isShadowMapIdxActive(i)) continue;
                const Ogre::Light *l = sn->getLightAssociatedWith(i);
                if (!l || l->getType() != Ogre::Light::LT_DIRECTIONAL) continue;
                const Ogre::Matrix4 vp = sn->getViewProjectionMatrix(i);
                // THE MAP'S OWN RECTANGLE of the atlas: getViewProjectionMatrix lands in IMAGE space with the map's
                // offset and length in it (clipToImageSpace x proj x view), never [-1, 1] (the verb tested a box
                // against [-1, 1] there - every map counted everything within a whole image of its rectangle).
                const Ogre::ShadowTextureDefinition *td = sn->getDefinition()->getShadowTextureDefinition(i);
                const float u0 = float(td->uvOffset.x), u1 = u0 + float(td->uvLength.x);
                const float v0 = float(td->uvOffset.y), v1 = v0 + float(td->uvLength.y);
                ShadowRouteShare share;
                share.map = unsigned(i);
                for (Node *nd : mItemNodes) {
                    if (!nd || !nd->item || !nd->shown || !nd->item->getCastShadows()) continue;
                    const Ogre::uint32 flags = gpuFlagsFor(*nd);
                    if (!(flags & kGpuCaster)) continue;
                    const Ogre::Aabb box = nd->item->getWorldAabbUpdated();
                    const Ogre::Vector3 lo = box.getMinimum(), hi = box.getMaximum();
                    Ogre::Vector2 a(1e30f, 1e30f), b(-1e30f, -1e30f);
                    for (int c = 0; c < 8; ++c) {
                        const Ogre::Vector3 p((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
                        const Ogre::Vector4 q = vp * Ogre::Vector4(p.x, p.y, p.z, 1.0f);
                        const float w = std::fabs(float(q.w)) > 1e-12f ? float(q.w) : 1.0f;
                        a.x = std::min(a.x, float(q.x) / w); a.y = std::min(a.y, float(q.y) / w);
                        b.x = std::max(b.x, float(q.x) / w); b.y = std::max(b.y, float(q.y) / w);
                    }
                    if (b.x < u0 || a.x > u1 || b.y < v0 || a.y > v1) continue;
                    const AtomRoute r = atomRouteFor(*nd, flags);
                    // A CASTER THE CUT DRAWS while its colour stays on PBS (stage 2) is counted as that, not as its reason.
                    const unsigned route = (r != AtomRoute::Atom && (flags & kGpuAtomCaster))
                                               ? ShadowRouteShare::kAtomCaster
                                               : std::min(unsigned(r), ShadowRouteShare::kAtomCaster - 1u);
                    ++share.items[route];
                    share.triangles[route] += itemLevel0Triangles(*nd->item);
                }
                out.push_back(share);
            }
            return true;
        }
        return false;
    } JAH_CATCH(mError, false);
}

// THE COVERAGE A/B (Scene::coverageSnapshot / coverageCompare; ENGINE-POLISH-ATOM-1 stage 3's silhouette bound, a
// measuring door): the first view's FINAL depth (the id pass's depth target, which the scene passes after it test and
// write: Atom and stock items alike) and its sun maps, read back synchronously (Image2 - a measurement's read, never a
// frame's) into one of two snapshots; the two compared pixel by pixel.
namespace {
/// A depth texel's float (a D32 or a D32S8 texel's first four bytes: the box strides by the format).
float depthTexel(const Ogre::TextureBox &box, unsigned x, unsigned y) {
    float v = 0.0f;
    std::memcpy(&v, static_cast<const unsigned char *>(box.at(x, y, 0)), sizeof(float));
    return v;
}
/// The bands of a mismatch bitmap: per mismatch, the shorter of its horizontal and vertical run.
void bandsOf(const std::vector<unsigned char> &m, unsigned w, unsigned h, std::vector<float> &out) {
    std::vector<unsigned> hr(m.size(), 0u), vr(m.size(), 0u);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w;) {
            if (!m[size_t(y) * w + x]) { ++x; continue; }
            unsigned e = x;
            while (e < w && m[size_t(y) * w + e]) ++e;
            for (unsigned k = x; k < e; ++k) hr[size_t(y) * w + k] = e - x;
            x = e;
        }
    for (unsigned x = 0; x < w; ++x)
        for (unsigned y = 0; y < h;) {
            if (!m[size_t(y) * w + x]) { ++y; continue; }
            unsigned e = y;
            while (e < h && m[size_t(e) * w + x]) ++e;
            for (unsigned k = y; k < e; ++k) vr[size_t(k) * w + x] = e - y;
            y = e;
        }
    out.clear();
    for (size_t i = 0; i < m.size(); ++i)
        if (m[i]) out.push_back(float(std::min(hr[i], vr[i])));
    std::sort(out.begin(), out.end());
}
void percentiles(const std::vector<float> &b, float &p50, float &p99, float &mx) {
    p50 = b.empty() ? 0.0f : b[size_t(double(b.size() - 1) * 0.50)];
    p99 = b.empty() ? 0.0f : b[size_t(double(b.size() - 1) * 0.99)];
    mx = b.empty() ? 0.0f : b.back();
}
}   // namespace

bool OgreScene::coverageSnapshot(unsigned slot)
{
    if (hostPosts()) { bool r = false; requestOnEngine([&] { r = coverageSnapshot(slot); }); return r; }
    JAH_HOST_CALL("Scene::coverageSnapshot");
    if (slot > 1u) return false;
    CoverageShot &shot = mCoverageShots[slot];
    shot = CoverageShot();
    JAH_TRY {
        for (OgreView *v : mEngine ? mEngine->viewsOf(this) : std::vector<OgreView *>()) {
            Ogre::TextureGpu *depth = v ? v->atomReceiverDepth().depth : nullptr;
            if (!depth) continue;
            const unsigned *rect = v->atomReceiverDepth().rect;
            Ogre::Image2 img;
            img.convertFromTexture(depth, 0u, 0u);
            const Ogre::TextureBox box = img.getData(0u);
            shot.w = rect[2];
            shot.h = rect[3];
            // THE DEPTH AS A VIEW DISTANCE, by the camera's own depth-range projection (perspective or orthographic
            // alike: clip z = P22 z + P23, clip w = P32 z + P33 over the view's z).
            shot.proj = v->camera() ? v->camera()->getProjectionMatrixWithRSDepth() : Ogre::Matrix4::IDENTITY;
            shot.depth.assign(size_t(shot.w) * shot.h, 0.0f);
            for (unsigned y = 0; y < shot.h; ++y)
                for (unsigned x = 0; x < shot.w; ++x)
                    shot.depth[size_t(y) * shot.w + x] = depthTexel(box, rect[0] + x, rect[1] + y);
            // THE CUT-OUTS' PIXELS: the id image's slots (x words) whose flags say the id pass drew them masked.
            shot.masked.assign(size_t(shot.w) * shot.h, 0u);
            Ogre::CompositorWorkspace *ws0 = v->workspace();
            Ogre::TextureGpu *idTex = nullptr;
            for (Ogre::CompositorNode *n : ws0 ? ws0->getNodeSequence() : Ogre::CompositorNodeVec())
                if ((idTex = n->getDefinedTexture(Ogre::IdString(kAtomIdTexture))) != nullptr) break;
            const detail::GpuInstance *mirror = mGpuScene.live() ? mGpuScene.mirrorData() : nullptr;
            if (idTex && mirror) {
                Ogre::Image2 ii;
                ii.convertFromTexture(idTex, 0u, 0u);
                const Ogre::TextureBox ib = ii.getData(0u);
                for (unsigned y = 0; y < shot.h && rect[1] + y < idTex->getHeight(); ++y)
                    for (unsigned x = 0; x < shot.w && rect[0] + x < idTex->getWidth(); ++x) {
                        uint32_t idx = 0u;
                        std::memcpy(&idx, static_cast<const unsigned char *>(ib.at(rect[0] + x, rect[1] + y, 0)),
                                    sizeof(idx));
                        if (idx == 0xFFFFFFFFu) continue;
                        const uint32_t slot = idx & 0x00FFFFFFu;
                        if (slot >= mGpuScene.slotCount()) continue;
                        uint32_t f = 0u;
                        std::memcpy(&f, &mirror[slot].boundsMax[3], sizeof(f));
                        shot.masked[size_t(y) * shot.w + x] = (f & kGpuAtom) && (f & kGpuAlphaTested);
                    }
            }
            Ogre::CompositorWorkspace *ws = v->workspace();
            Ogre::CompositorShadowNode *sn = ws ? ws->findShadowNode(Ogre::IdString(OgreView::kShadowNodeName)) : nullptr;
            Ogre::TextureGpu *atlas = sn ? sn->getDefinedTexture(Ogre::IdString("atlas0")) : nullptr;
            if (atlas) {
                Ogre::Image2 ai;
                ai.convertFromTexture(atlas, 0u, 0u);
                const Ogre::TextureBox ab = ai.getData(0u);
                const unsigned tw = atlas->getWidth(), th = atlas->getHeight();
                const Ogre::CompositorShadowNodeDef *def = sn->getDefinition();
                for (size_t i = 0; i < def->getNumShadowTextureDefinitions(); ++i) {
                    if (!sn->isShadowMapIdxActive(i)) continue;
                    const Ogre::Light *l = sn->getLightAssociatedWith(i);
                    if (!l || l->getType() != Ogre::Light::LT_DIRECTIONAL) continue;
                    const Ogre::ShadowTextureDefinition *td = def->getShadowTextureDefinition(i);
                    CoverageShot::Map m;
                    m.map = unsigned(i);
                    const unsigned x0 = unsigned(td->uvOffset.x * Ogre::Real(tw) + Ogre::Real(0.5));
                    const unsigned y0 = unsigned(td->uvOffset.y * Ogre::Real(th) + Ogre::Real(0.5));
                    m.w = unsigned(td->uvLength.x * Ogre::Real(tw) + Ogre::Real(0.5));
                    m.h = unsigned(td->uvLength.y * Ogre::Real(th) + Ogre::Real(0.5));
                    m.vp = sn->getViewProjectionMatrix(i);
                    m.atlasW = tw;
                    m.d.assign(size_t(m.w) * m.h, 0.0f);
                    for (unsigned y = 0; y < m.h && y0 + y < th; ++y)
                        for (unsigned x = 0; x < m.w && x0 + x < tw; ++x)
                            m.d[size_t(y) * m.w + x] = depthTexel(ab, x0 + x, y0 + y);
                    shot.maps.push_back(std::move(m));
                }
            }
            shot.valid = true;
            return true;
        }
        return false;
    } JAH_CATCH(mError, false);
}

bool OgreScene::coverageCompare(float viewRelDepth, float mapDepthTexels, bool maskedOnly, CoverageCompare &out)
{
    if (hostPosts()) {
        bool r = false;
        requestOnEngine([&] { r = coverageCompare(viewRelDepth, mapDepthTexels, maskedOnly, out); });
        return r;
    }
    JAH_HOST_CALL("Scene::coverageCompare");
    out = CoverageCompare();
    const CoverageShot &a = mCoverageShots[0], &b = mCoverageShots[1];
    if (!a.valid || !b.valid || a.w != b.w || a.h != b.h) return false;
    out.valid = true;
    out.width = a.w;
    out.height = a.h;
    std::vector<float> bands;
    {
        // THE VIEW DISTANCES (the snapshot's own projection: z = (P23 - d P33) / (d P32 - P22)); two differ by more
        // than `viewRelDepth` of the nearer — a cleared (sky) pixel, at infinity or the far plane, against a surface
        // included.
        std::vector<unsigned char> m(a.depth.size(), 0u);
        const auto dist = [](const Ogre::Matrix4 &P, float d) {
            const double den = double(d) * double(P[3][2]) - double(P[2][2]);
            return std::fabs(den) > 1e-30 ? float(std::fabs((double(P[2][3]) - double(d) * double(P[3][3])) / den))
                                          : 3.0e38f;
        };
        for (size_t i = 0; i < m.size(); ++i) {
            const float da = a.depth[i], db = b.depth[i];
            const float za = dist(a.proj, da), zb = dist(b.proj, db);
            m[i] = da != db && std::fabs(za - zb) > viewRelDepth * std::min(za, zb);
            if (maskedOnly && !(i < a.masked.size() && a.masked[i]) && !(i < b.masked.size() && b.masked[i])) m[i] = 0u;
            out.viewMismatch += m[i];
        }
        bandsOf(m, a.w, a.h, bands);
        percentiles(bands, out.viewBandP50, out.viewBandP99, out.viewBandMax);
    }
    for (const CoverageShot::Map &ma : a.maps)
        for (const CoverageShot::Map &mb : b.maps) {
            if (ma.map != mb.map || ma.w != mb.w || ma.h != mb.h) continue;
            CoverageCompare::Map cm;
            cm.map = ma.map;
            cm.width = ma.w;
            cm.height = ma.h;
            float diff = 0.0f;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) diff = std::max(diff, float(std::fabs(ma.vp[r][c] - mb.vp[r][c])));
            cm.sameLight = diff < 1e-4f;
            // ORTHOGRAPHIC (a sun map): a depth difference is a world distance along the light through row 2, counted
            // in the map's texel footprints (shadow.atom_parity's metric: row 0's length, without the atlas's
            // translation, over the atlas's width — the map's x scale is its rectangle's share of it).
            const float r2 = std::sqrt(float(ma.vp[2][0] * ma.vp[2][0] + ma.vp[2][1] * ma.vp[2][1] +
                                             ma.vp[2][2] * ma.vp[2][2]));
            const float r0 = std::sqrt(float(ma.vp[0][0] * ma.vp[0][0] + ma.vp[0][1] * ma.vp[0][1] +
                                             ma.vp[0][2] * ma.vp[0][2]));
            const float texelWorld = ma.atlasW && r0 > 0.0f ? 1.0f / (r0 * float(ma.atlasW)) : 1.0f;
            std::vector<unsigned char> m(ma.d.size(), 0u);
            for (size_t i = 0; i < m.size(); ++i) {
                m[i] = std::fabs(ma.d[i] - mb.d[i]) / std::max(1e-12f, r2) > mapDepthTexels * texelWorld;
                cm.mismatch += m[i];
            }
            bandsOf(m, ma.w, ma.h, bands);
            percentiles(bands, cm.bandP50, cm.bandP99, cm.bandMax);
            out.maps.push_back(cm);
        }
    return true;
}

void OgreScene::setShadowReceiverCull(bool on)
{
    if (postScene([=](OgreScene &s) { s.setShadowReceiverCull(on); return true; })) return;   // U3 4a: a message
    JAH_HOST_CALL("Scene::setShadowReceiverCull");
    mShadowReceiverCull = on;
}

}}}   // namespace jahshaka::engine::detail
