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
void OgreEngine::buildShadowNode(const char *name, unsigned baseResolution, unsigned focusedMaps)
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
    //   per map: one clear quad                                   (3 + N)
    //   PSSM:    one scene pass per split                         (3)
    //   focused: one scene pass (spot) + 6 cube faces + 1 copy     (N * 8)
    def->setNumTargetPass(numMaps + 3u + N * 8u);

    const Ogre::uint8 directionalMask = Ogre::uint8(1u << Ogre::Light::LT_DIRECTIONAL);
    const Ogre::uint8 pointMask       = Ogre::uint8(1u << Ogre::Light::LT_POINT);
    const Ogre::uint8 spotMask        = Ogre::uint8(1u << Ogre::Light::LT_SPOTLIGHT);
    const char *clearMaterial = shadowClearMaterialName();

    // (a) THE PER-MAP CLEARS, first, in map order. Each one is gated by
    //     CompositorNode::_update on ITS OWN map index: it runs when the map
    //     holds a light and that light is dynamic or a dirty static
    //     (_shouldUpdateShadowMapIdx), which is precisely the frames in which
    //     the map is about to be re-rendered. A static map that is clean is
    //     neither cleared nor drawn, so its contents survive — the whole point.
    for (unsigned m = 0u; m < numMaps; ++m) {
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
    rebuildShadowAtlas(res, mShadowMapCount);
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
bool OgreEngine::rebuildShadowAtlas(unsigned resolution, unsigned focusedMaps) {
    const unsigned res = std::min(8192u, std::max(256u, resolution));
    const unsigned maps = std::min(kMaxShadowMaps, std::max(2u, focusedMaps));
    if (res == mShadowResolution && maps == mShadowMapCount && mHlmsRegistered) return false;
    mShadowResolution = res;
    mShadowMapCount = maps;
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
        if (cm->hasShadowNodeDefinition(OgreView::kShadowNodeName))
            cm->removeShadowNodeDefinition(OgreView::kShadowNodeName);
        if (cm->hasShadowNodeDefinition(OgreView::kReflectShadowNodeName))
            cm->removeShadowNodeDefinition(OgreView::kReflectShadowNodeName);
        createShadowNode();
        for (OgreView *v : rebuilt) v->recreateWorkspaceAfterShadowRebuild();
        for (OgreScene *s : planarRebuilt) s->recreatePlanarAfterShadowRebuild();
        ok = true;
    } JAH_CATCH(mLastError, false);
    return ok;
}

}}}   // namespace jahshaka::engine::detail
