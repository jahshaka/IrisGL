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

#include <Compositor/Pass/PassClear/OgreCompositorPassClearDef.h>
#include <Compositor/Pass/PassQuad/OgreCompositorPassQuadDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreDepthBuffer.h>

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
                                 bool perMapClears)
{
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    const Ogre::RenderSystemCapabilities *caps = rs->getCapabilities();
    unsigned maxDim = caps ? unsigned(caps->getMaximumResolution2D()) : 16384u;
    if (maxDim == 0u) maxDim = 16384u;
    maxDim = std::min(maxDim, 16384u);

    const ShadowAtlasPlan plan =
        planShadowAtlas(baseResolution, std::max(2u, focusedMaps), maxDim);
    const unsigned N = plan.focusedMaps;

    Ogre::CompositorShadowNodeDef *def = cm->addShadowNodeDefinition(name);

    // ---- textures -----------------------------------------------------
    // The atlas: one depth texture with an explicit RTV, exactly as the helper
    // declares it (a depth format + NO_POOL_EXPLICIT_RTV is what makes the
    // shadow maps SAMPLEABLE depth rather than a pooled depth buffer).
    def->setNumLocalTextureDefinitions(2u);   // atlas0 + tmpCubemap
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
        Ogre::RenderTargetViewDef *rtv = def->addRenderTextureView(atlasName);
        rtv->setForTextureDefinition(atlasName, texDef);
    }
    // The point-light scratch cubemap (six faces rendered, then copied into the
    // light's rectangle of the atlas as a dual-paraboloid map).
    const Ogre::String cubeName = "tmpCubemap";
    {
        Ogre::TextureDefinitionBase::TextureDefinition *texDef = def->addTextureDefinition(cubeName);
        texDef->width  = kPointLightCubemapResolution;
        texDef->height = kPointLightCubemapResolution;
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

    const Ogre::uint8 directionalMask = Ogre::uint8(1u << Ogre::Light::LT_DIRECTIONAL);
    const Ogre::uint8 pointMask       = Ogre::uint8(1u << Ogre::Light::LT_POINT);
    const Ogre::uint8 spotMask        = Ogre::uint8(1u << Ogre::Light::LT_SPOTLIGHT);
    const char *clearMaterial = shadowClearMaterialName();

    // (a) THE CLEAR. Upstream's ONE whole-atlas clear while no shadow map in
    //     this process is static, and one QUAD PER MAP once one is — because
    //     the quads are not free and, until a user ticks Static Shadow, they
    //     buy nothing.
    //
    //     MEASURED, which is why this is a switch and not a constant
    //     (gi.coalesce, 2026-09-09): three quads over the PSSM block (2048^2 +
    //     two 1024^2 of depth writes) every frame, where a hardware fast-clear
    //     had covered the whole atlas, delayed the frame enough to flip a
    //     neighbouring GI suite's frame-phase 3 times in 6 runs. With this
    //     switch a scene that uses no static maps executes exactly upstream's
    //     pass list again, and the suite is 6/6.
    //
    //     The engine rebuilds the node when the answer changes (the same
    //     drop-swap-recreate a Shadow Quality change costs), so the quads exist
    //     exactly in the sessions that need them.
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
    //     the map is about to be re-rendered. A static map that is clean is
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
        target->setNumPasses(1u);
        Ogre::CompositorPassSceneDef *scene =
            static_cast<Ogre::CompositorPassSceneDef *>(target->addPass(Ogre::PASS_SCENE));
        scene->mShadowMapIdx = j;
        scene->mFirstRQ = 0u;
        scene->mLastRQ = 255u;
        scene->mIncludeOverlays = false;
        scene->mVisibilityMask = kVisibleBit;
    }

    // (c) The focused maps: a spot pass, then the point-light cubemap + copy.
    //     Upstream renders every directional/spot pass before every point pass;
    //     the order between maps does not matter here because each map clears
    //     itself, but keeping it makes the two definitions comparable.
    for (unsigned i = 0u; i < N; ++i) {
        Ogre::CompositorTargetDef *target = def->addTargetPass(atlasName);
        target->setShadowMapSupportedLightTypes(spotMask);
        target->setNumPasses(1u);
        Ogre::CompositorPassSceneDef *scene =
            static_cast<Ogre::CompositorPassSceneDef *>(target->addPass(Ogre::PASS_SCENE));
        scene->mShadowMapIdx = 3u + i;
        scene->mFirstRQ = 0u;
        scene->mLastRQ = 255u;
        scene->mIncludeOverlays = false;
        scene->mVisibilityMask = kVisibleBit;
    }
    for (unsigned i = 0u; i < N; ++i) {
        for (Ogre::uint32 face = 0u; face < 6u; ++face) {
            Ogre::CompositorTargetDef *target = def->addTargetPass(cubeName, face);
            target->setShadowMapSupportedLightTypes(pointMask);
            target->setNumPasses(1u);
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
            scene->mVisibilityMask = kVisibleBit;
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
    mShadowFilter = f;
    if (mHlmsRegistered) applyShadowFilter();
    // else: applied by ensureHlms() once the Hlms exists.
}

ShadowFilter OgreEngine::shadowFilter() const { return mShadowFilter; }

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
    const unsigned res = std::min(8192u, std::max(256u, pixels));
    if (res == mShadowResolution) return;
    rebuildShadowAtlas(res, mShadowMapCount, mShadowPerMapClears);
}

unsigned OgreEngine::shadowResolution() const { return mShadowResolution; }

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
    const unsigned res = std::min(8192u, std::max(256u, resolution));
    const unsigned maps = std::min(kMaxShadowMaps, std::max(2u, focusedMaps));
    if (res == mShadowResolution && maps == mShadowMapCount && clears == mShadowPerMapClears &&
        mHlmsRegistered)
        return false;
    mShadowResolution = res;
    mShadowMapCount = maps;
    mShadowPerMapClears = clears;
    if (!mHlmsRegistered) return false;   // first createShadowNode() picks it up
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
        createShadowNode();
        for (OgreView *v : rebuilt) v->recreateWorkspaceAfterShadowRebuild();
        for (OgreScene *s : planarRebuilt) s->recreatePlanarAfterShadowRebuild();
        for (OgreScene *s : giRebuilt) s->recreateGiAfterShadowRebuild();
        ok = true;
    } JAH_CATCH(mLastError, false);
    return ok;
}

// ---------------------------------------------------------------------------
// The derived count (SHADOW_TOOLING_SPEC.md §4.1)
// ---------------------------------------------------------------------------
void OgreEngine::setShadowMapBudget(unsigned maps) {
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

void OgreEngine::deriveShadowMapCount() {
    if (!mHlmsRegistered || mHeadless) return;
    // What the frame is about to draw — the same set the frame loop updates, so
    // a preview scene nobody is looking at cannot force the editor's atlas to
    // grow.
    std::vector<OgreScene *> scenes;
    scenesFeedingEnabledViews(scenes);
    unsigned casters = 0;
    for (OgreScene *s : scenes) casters = std::max(casters, s->countLocalShadowCasters(nullptr));

    const unsigned budget = effectiveShadowMapBudget();
    const unsigned want = std::min(stepShadowMapCount(casters), budget);

    // THE EXCEEDED CASE, said out loud once. Ogre's own behaviour (keep the
    // closest casters, drop the rest) is unchanged; what changes is that it
    // stops being silent. The host repeats it in the World panel and through
    // shadowStatus().
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

    if (want <= mShadowMapCount) {          // never shrink in-session (D4)
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
    Ogre::LogManager::getSingleton().logMessage(
        "Jahshaka shadows: growing the atlas to " + Ogre::StringConverter::toString(want) +
        " focused shadow maps for " + Ogre::StringConverter::toString(casters) + " casters");
    rebuildShadowAtlas(mShadowResolution, want, mShadowPerMapClears);
}

// ---------------------------------------------------------------------------
// The readback
// ---------------------------------------------------------------------------
ShadowStatus OgreEngine::shadowStatus() const {
    ShadowStatus st;
    st.requestedBudget = mShadowMapBudget;
    if (!mHlmsRegistered || mHeadless) return st;   // live stays false
    // ASKING TURNS THE COUNTERS ON, exactly like RenderStats::metricsRecording:
    // the shadow-pass listener is not free (a callback per compositor pass per
    // frame), so it only runs while somebody is reading it or while a static
    // map exists. The FIRST call therefore reports 0 passes; every call after a
    // rendered frame reports the truth.
    mShadowStatusPolled = true;
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
        st.atlasBytes = plan.bytes();
        st.focusedMaps = plan.focusedMaps;
        st.maps = st.pssmSplits + st.focusedMaps;
        st.lightSlots = 1u + st.focusedMaps;
        st.live = true;
        st.shadowPassesLastFrame = mShadowPassesLastFrame;
        st.staticMapRendersLastFrame = mStaticShadowRendersLastFrame;

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
                    info.isStatic = lights[i].isStatic;
                    info.dirty = lights[i].isDirty;
                    if (info.node) seen.push_back(info.node);
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
// Static shadow maps (SHADOW_TOOLING_SPEC.md §4.3)
// ---------------------------------------------------------------------------
// A static map is rendered ONCE and then skipped until it is dirtied: Ogre's
// `_shouldUpdateShadowMapIdx` returns false for a clean static slot, which
// skips its scene pass, its six cube faces, its DPSM copy — and, because our
// definition clears per map rather than per atlas, its CLEAR as well. That last
// one is the whole reason phase 0 existed: with upstream's whole-atlas clear the
// contents would be wiped every frame and "not re-rendering" would mean "black".
//
// THE SLOT RULE (F7, OgreCompositorShadowNode.h:308-317). Fixed lights are tied
// to the END of the focused range and Ogre's distance sort fills from the front,
// so a static light never steals the slot the sort was about to use, and the
// dynamic casters keep the closest-first behaviour they always had.
//
// PER WORKSPACE, NOT PER DEFINITION (F8). mShadowMapCastingLights and the dirty
// flags are instance state, so every view's shadow node has to be told
// separately — and a workspace recreated by an atlas rebuild starts empty,
// which the "assign when it differs" test below picks up on the next frame.

/// The shadow-node pass counter. Counts passes whose parent node IS the shadow
/// node, per shadow-map index, so "a static map rendered this frame" is a
/// measurement and not an inference.
class OgreEngine::ShadowPassCounter final : public Ogre::CompositorWorkspaceListener {
public:
    void workspacePreUpdate(Ogre::CompositorWorkspace *) override { reset(); }
    void passPreExecute(Ogre::CompositorPass *pass) override {
        const Ogre::CompositorNode *node = pass->getParentNode();
        if (!node || node->getName() != Ogre::IdString(OgreView::kShadowNodeName)) return;
        ++mTotal;
        const Ogre::uint32 idx = pass->getDefinition()->mShadowMapIdx;
        if (idx < kMaxTrackedMaps) ++mPerMap[idx];
    }
    void reset() { mTotal = 0; for (unsigned &c : mPerMap) c = 0; }
    unsigned total() const { return mTotal; }
    unsigned perMap(unsigned idx) const { return idx < kMaxTrackedMaps ? mPerMap[idx] : 0u; }

private:
    static constexpr unsigned kMaxTrackedMaps = 3u + kMaxShadowMaps;
    unsigned mTotal = 0;
    unsigned mPerMap[kMaxTrackedMaps] = { 0 };
};

void OgreEngine::detachShadowCounter() {
    if (mShadowCounterView && mShadowCounter)
        mShadowCounterView->removeWorkspaceListener(mShadowCounter);
    mShadowCounterView = nullptr;
    delete mShadowCounter;
    mShadowCounter = nullptr;
}

void OgreEngine::noteViewDestroyed(OgreView *view) {
    // THE COUNTER RIDES A VIEW, and this is where that view can die. Without
    // this the next frame's applyStaticShadowMaps dereferences a freed OgreView
    // to detach its listener — found by ASan on test_engine_asan's
    // shadow_resolution_rebuilds_the_atlas, which destroys views while the
    // counter is attached.
    if (!view || view != mShadowCounterView) return;
    if (mShadowCounter) view->removeWorkspaceListener(mShadowCounter);
    mShadowCounterView = nullptr;
}

bool OgreEngine::refreshShadows() {
    if (!mHlmsRegistered || mHeadless) return false;
    bool any = false;
    for (auto &s : mScenes) { s->dirtyStaticShadows(); any = true; }
    mRefreshShadowsPending = any;
    return any;
}

void OgreEngine::applyStaticShadowMaps() {
    if (!mHlmsRegistered || mHeadless) return;
    JAH_TRY {
        // The counter rides the first enabled view that has a shadow node —
        // the same "one view speaks for the process" rule the HUD and the post
        // chain's recompile globals use. It is re-attached rather than hooked
        // up once because a view's workspace is dropped and recreated by every
        // atlas rebuild, and addWorkspaceListener is a no-op on repeat.
        // THE COUNTER IS OPT-IN, and that is a performance decision, not a
        // preference (found by gi.coalesce, 2026-09-09). A
        // CompositorWorkspaceListener is called back for EVERY compositor pass
        // of EVERY frame, and a GI scene has a lot of passes: attaching it
        // unconditionally put a virtual call plus an IdString compare on the
        // render path of every scene in the process, for numbers almost nobody
        // reads. It measurably moved a neighbouring suite's frame-phase.
        //
        // So it rides the RenderStats::metricsRecording pattern this engine
        // already uses: recording starts when something asks (shadowStatus())
        // or when there is something to measure (a static map exists), and
        // stops again when neither holds.
        const bool wantCounter = mShadowStatusPolled || mShadowStaticSeen;
        if (wantCounter && !mShadowCounter) mShadowCounter = new ShadowPassCounter();
        OgreView *counterView = nullptr;

        std::vector<OgreScene *> scenes;
        scenesFeedingEnabledViews(scenes);

        // THE EARLY OUT, and the reason it exists: this function is on the
        // render path of EVERY frame of every scene in the process, and the
        // overwhelmingly common case is "no light in this process has a static
        // shadow map" — every scene shipped today. In that case there is
        // nothing to assign, nothing to dirty and nothing to count, and the
        // work below (a walk of the views, findShadowNode per view, the fixed
        // -light table per view) is pure overhead. `mShadowStaticSeen` keeps
        // one frame of memory so the LAST static light's slot is still released
        // properly before the early-out engages.
        bool anyStatic = false;
        for (OgreScene *s : scenes) if (s->hasStaticShadowLights()) { anyStatic = true; break; }
        // THE CLEAR STRATEGY FOLLOWS THE NEED. Per-map clear quads exist to let
        // a static map survive; until one exists they are pure cost (measured —
        // see buildShadowNode). The first static light in the process rebuilds
        // the node with them, which is the same hitch a Shadow Quality change
        // costs and happens once.
        if (anyStatic != mShadowPerMapClears)
            rebuildShadowAtlas(mShadowResolution, mShadowMapCount, anyStatic);
        if (!anyStatic && !mShadowStaticSeen && !mShadowStatusPolled) {
            for (OgreScene *s : scenes) s->takeStaticShadowsDirty();
            mShadowPassesLastFrame = 0;
            mStaticShadowRendersLastFrame = 0;
            return;
        }

        // Consume each scene's dirty flag ONCE for the whole frame, before the
        // per-view loop: two views of the same scene must not make the second
        // one miss the dirty (or, worse, dirty a map that has already been
        // re-rendered this frame).
        std::vector<std::pair<OgreScene *, bool>> dirty;
        dirty.reserve(scenes.size());
        for (OgreScene *s : scenes) {
            // Rule 1 (the light moved) is checked here rather than pushed from
            // outside, so a host that drives the engine directly still gets
            // correct static shadows — see OgreScene::staticLightsMoved.
            const bool moved = s->staticLightsMoved();
            dirty.emplace_back(s, s->takeStaticShadowsDirty() || moved);
        }
        mRefreshShadowsPending = false;

        unsigned staticSlots = 0;
        bool sawStatic = false;
        for (auto &v : mViews) {
            if (!v->isEnabled() || !v->ogreScene()) continue;
            Ogre::CompositorShadowNode *node = v->shadowNodeInstance();
            if (!node) continue;
            if (!counterView && wantCounter) counterView = v.get();

            if (!wantCounter) counterView = nullptr;
            OgreScene *scene = v->ogreScene();
            bool sceneDirty = false;
            for (const auto &d : dirty) if (d.first == scene) sceneDirty = d.second;

            std::vector<std::pair<NodeId, Ogre::Light *>> statics;
            scene->staticShadowLights(statics);
            // Never more static lights than there are focused maps, and never
            // ALL of them: a scene where every caster is static would leave the
            // dynamic sort nothing to work with, which is legal but surprising
            // — the cap is the map count itself, and the lights that do not fit
            // simply stay dynamic (reported through shadowStatus).
            if (statics.size() > mShadowMapCount) statics.resize(mShadowMapCount);
            staticSlots = std::max(staticSlots, unsigned(statics.size()));
            if (!statics.empty()) sawStatic = true;

            const Ogre::LightClosestArray &held = node->getShadowCastingLights();
            // Walk the focused slots from the BACK, handing out the last ones to
            // the static lights in order (F7).
            for (unsigned i = 0; i < mShadowMapCount; ++i) {
                const unsigned slot = mShadowMapCount - i;      // light slot: 1..N
                if (slot >= held.size() + 1u) continue;
                const unsigned mapIdx = slot + 2u;              // 3 PSSM splits first
                Ogre::Light *want = i < statics.size() ? statics[i].second : nullptr;
                const bool isStatic = held[slot].isStatic;
                Ogre::Light *have = isStatic ? held[slot].light : nullptr;
                if (want != have) {
                    // setLightFixedToShadowMap(idx, null) releases the slot back
                    // to the dynamic sort; with a light it also marks it dirty,
                    // which is why this must only run when the ASSIGNMENT
                    // changed — calling it every frame would keep the map
                    // permanently dirty and save nothing at all.
                    node->setLightFixedToShadowMap(mapIdx, want);
                } else if (want && sceneDirty) {
                    // includeLinked=false: our maps clear individually, so one
                    // dirty map does not oblige its atlas neighbours to redraw
                    // (which is exactly what upstream's whole-atlas clear WOULD
                    // have obliged, and why upstream defaults it to true).
                    node->setStaticShadowMapDirty(mapIdx, false);
                }
            }
        }

        mShadowStaticSeen = sawStatic;

        // Attribute last frame's counts. The per-map numbers were collected by
        // the listener during the PREVIOUS frame, so this reads them before the
        // next workspacePreUpdate resets them.
        if (mShadowCounter) {
            mShadowPassesLastFrame = mShadowCounter->total();
            unsigned staticRenders = 0;
            for (unsigned i = 0; i < staticSlots; ++i) {
                const unsigned mapIdx = mShadowMapCount - i + 2u;
                staticRenders += mShadowCounter->perMap(mapIdx);
            }
            mStaticShadowRendersLastFrame = staticRenders;
        } else {
            mShadowPassesLastFrame = 0;
            mStaticShadowRendersLastFrame = 0;
        }

        if (counterView != mShadowCounterView) {
            // BELT AND BRACES over noteViewDestroyed's unhook: only ever detach
            // from a view that is still in mViews. A raw view pointer held
            // across frames is exactly the shape of the use-after-free ASan
            // caught here once, and the check costs a walk of a handful of
            // views.
            bool stillAlive = false;
            for (const auto &v : mViews) if (v.get() == mShadowCounterView) stillAlive = true;
            if (mShadowCounterView && stillAlive)
                mShadowCounterView->removeWorkspaceListener(mShadowCounter);
            mShadowCounterView = counterView;
        }
        if (mShadowCounterView && mShadowCounter)
            mShadowCounterView->addWorkspaceListener(mShadowCounter);
        // Nothing wants the numbers any more: unhook and let the render path go
        // back to costing nothing.
        if (!wantCounter && mShadowCounter) detachShadowCounter();
    } JAH_CATCH(mLastError, );
}

}}}   // namespace jahshaka::engine::detail
