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

    // What this node's maps draw: ONE definition shared with the lamp-map
    // cache's caster scan (shadowCasterChannels — R1/R2 widen it per kind).
    const ShadowNodeKind kind =
        Ogre::IdString(name) == Ogre::IdString(OgreView::kReflectShadowNodeName) ? ShadowNodeKind::Reflect
        : Ogre::IdString(name) == Ogre::IdString(OgreView::kProbeShadowNodeName) ? ShadowNodeKind::Probe
                                                                                  : ShadowNodeKind::View;
    const Ogre::uint32 casterMask = shadowCasterChannels(kind);
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
        target->setNumPasses(1u);
        Ogre::CompositorPassSceneDef *scene =
            static_cast<Ogre::CompositorPassSceneDef *>(target->addPass(Ogre::PASS_SCENE));
        scene->mShadowMapIdx = j;
        scene->mFirstRQ = 0u;
        scene->mLastRQ = 255u;
        scene->mIncludeOverlays = false;
        scene->mVisibilityMask = casterMask;
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
        scene->mVisibilityMask = casterMask;
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
            scene->mVisibilityMask = casterMask;
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
    if (mHeadless) return false;   // no pixels, no atlases (the siblings gate the same way)
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
        if (cm->hasShadowNodeDefinition(OgreView::kProbeShadowNodeName))
            cm->removeShadowNodeDefinition(OgreView::kProbeShadowNodeName);
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
    // the shadow-pass listeners are not free (a callback per compositor pass
    // per frame), so they run only while somebody is reading — and the asking
    // EXPIRES: kShadowPollWindowFrames frames without a read detach them again
    // (P8; it used to be a latch that never cleared). The FIRST call therefore
    // reports 0 passes; every call after a rendered frame reports the truth.
    mShadowPolled = true;
    mShadowPollFrame = mShadowFrame;
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
        st.shadowPassesLastFrame = mShadowPassesLastFrame;
        st.cachedMapRendersLastFrame = mCachedMapRendersLastFrame;
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
        if (!mShadowPerMapClears) {
            std::vector<OgreScene *> scenes;
            scenesFeedingEnabledViews(scenes);
            bool anyLamp = false;
            for (OgreScene *s : scenes) {
                if (!s->hasCacheableShadowLights()) continue;
                bool shadowed = false;
                for (auto &v : mViews)
                    if (v->ogreScene() == s && v->shadowNodeInstance()) { shadowed = true; break; }
                std::vector<Ogre::CompositorWorkspace *> ws;
                s->shadowWorkspaces(ShadowNodeKind::Reflect, ws);
                s->shadowWorkspaces(ShadowNodeKind::Probe, ws);
                if (shadowed || !ws.empty()) { anyLamp = true; break; }
            }
            if (anyLamp) rebuildShadowAtlas(mShadowResolution, mShadowMapCount, true);
        }

        // THE COUNTERS' OPT-IN EXPIRES (P8): nobody asked for a while, so the
        // per-pass callbacks come off the render path and the readings go to 0.
        const bool want = mShadowPolled && mShadowFrame - mShadowPollFrame <= kShadowPollWindowFrames;
        if (!want) {
            mShadowPolled = false;
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
                if (held[slot].isStatic && held[slot].light == light)
                    n->setLightFixedToShadowMap(slot + 2u, nullptr);
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
std::vector<Ogre::Light *> planCachedSlots(const Ogre::LightClosestArray &held, size_t maps,
                                           const std::vector<Ogre::Light *> &want, size_t fixed) {
    std::vector<Ogre::Light *> plan(maps, nullptr);
    if (!fixed) return plan;
    const auto isSpot = [](const Ogre::Light *l) { return l->getType() == Ogre::Light::LT_SPOTLIGHT; };
    // 1. Keep every lamp that is still wanted where it is.
    std::vector<char> placed(want.size(), 0);
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
    if (ok) return plan;
    // 3. The canonical layout.
    std::fill(plan.begin(), plan.end(), nullptr);
    for (size_t i = 0; i < fixed && i < maps; ++i) plan[i] = want[i];
    return plan;
}
}   // namespace

void OgreEngine::applyShadowCacheDirties(const std::vector<OgreScene *> &drawn) {
    for (unsigned k = 0; k < kShadowNodeKinds; ++k)
        mShadowCachedInstances[k] = mShadowUncachedInstances[k] = mShadowDirtiedMaps[k] = 0;
    if (!mHlmsRegistered || mHeadless) return;
    JAH_TRY {
        // ---- the counters, armed for THIS frame (P8) ----------------------
        // Here and not at the top of the frame: applyPendingGi / applyPendingPlanar
        // may have rebuilt a probe or planar workspace since, and the counters
        // must ride the workspaces that are about to render.
        if (mShadowPolled) {
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
                    std::vector<Ogre::CompositorWorkspace *> ws;
                    s->shadowWorkspaces(ShadowNodeKind(k), ws);
                    for (Ogre::CompositorWorkspace *w : ws) attachOnce(w, mShadowCounters[k]);
                }
        }

        // ---- detection per scene, application per instance ----------------
        for (OgreScene *s : drawn) {
            struct Instance { Ogre::CompositorShadowNode *node; ShadowNodeKind kind; };
            std::vector<Instance> instances;
            // EVERY view of the scene, enabled or not: a disabled view keeps its
            // workspace, and marking its instance now is what keeps its maps
            // honest for the frame it is shown again (the flags persist).
            for (auto &v : mViews)
                if (v->ogreScene() == s)
                    if (Ogre::CompositorShadowNode *n = v->shadowNodeInstance())
                        instances.push_back({ n, ShadowNodeKind::View });
            for (unsigned k = 1u; k < kShadowNodeKinds; ++k) {
                std::vector<Ogre::CompositorWorkspace *> ws;
                s->shadowWorkspaces(ShadowNodeKind(k), ws);
                for (Ogre::CompositorWorkspace *w : ws)
                    if (Ogre::CompositorShadowNode *n = w->findShadowNode(shadowNodeNameOf(ShadowNodeKind(k))))
                        instances.push_back({ n, ShadowNodeKind(k) });
            }
            // THE FRAME'S ONE ITEM WALK: the GI movement records and — when
            // this scene has shadow nodes to cache into and lamps to cache — the
            // caster changes, in one pass on the bounds updateSceneGraph just
            // made current. A preview drawn with shadows off gets only the GI
            // half (if any); a node that appears later is new, and a new
            // assignment renders its maps regardless.
            s->runItemWalk(!instances.empty() && s->hasCacheableShadowLights());
            if (instances.empty()) continue;
            OgreScene::ShadowCacheFrame f;
            s->collectShadowCacheFrame(f);
            for (const Instance &in : instances) {
                const unsigned k = unsigned(in.kind);
                const Ogre::LightClosestArray &held = in.node->getShadowCastingLights();
                if (held.size() < 2u) continue;                  // a PSSM-only node
                const size_t maps = held.size() - 1u;            // light slots 1..maps
                std::vector<Ogre::Light *> want;
                want.reserve(f.lights.size());
                for (const OgreScene::ShadowCacheLight &l : f.lights)
                    if (shadowLampCachedFor(in.kind, l.light)) want.push_back(l.light);
                // Cache only with per-map clears (a whole-atlas clear would wipe
                // a cached map every frame) and only while every lamp fits.
                const bool cache = mShadowPerMapClears && !want.empty() && want.size() <= maps;
                if (cache) ++mShadowCachedInstances[k];
                else if (!want.empty()) ++mShadowUncachedInstances[k];
                const size_t fixed = cache ? want.size() : 0u;
                const std::vector<Ogre::Light *> &dirty = f.dirty[k];
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
                            std::find(want.begin(), want.end(), e.light) != want.end())
                            in.node->setLightFixedToShadowMap(j + 3u, nullptr);
                    }
                const std::vector<Ogre::Light *> plan = planCachedSlots(held, maps, want, fixed);
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
                        if (w) ++mShadowDirtiedMaps[k];
                    } else if (w && (f.dirtyAll ||
                                     std::find(dirty.begin(), dirty.end(), w) != dirty.end())) {
                        // includeLinked=false: our maps clear individually, so one
                        // dirty map does not oblige its atlas neighbours to redraw.
                        in.node->setStaticShadowMapDirty(mapIdx, false);
                        ++mShadowDirtiedMaps[k];
                    }
                }
            }
        }
    } JAH_CATCH(mLastError, );
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

}}}   // namespace jahshaka::engine::detail
