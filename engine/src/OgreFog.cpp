// Fog: Ogre's AtmosphereNpr adopted for its exponential fog MATH, wired to
// Jahshaka's authored colour and extended with a height layer.
//
// WHY A COMPONENT WE ONLY HALF WANT
// AtmosphereNpr bundles four things: a procedural sky quad, a fog model, a
// sun-light link and an ambient link. We want the second only. The component
// makes that separable, but the recipe is not obvious and is load-bearing:
//
//   setSky( sm, true );        // creates the sky Rectangle2D and registers us
//   setSky( sm, false );       // hides the quad (Rectangle2D honours setVisible,
//                              // unlike BillboardSet2) and UNregisters us
//   sm->_setAtmosphere( this ) // registers again — fog only, no visible sky
//
// The first call is not optional: AtmosphereNpr::_update() asserts on, and then
// dereferences, the per-SceneManager Rectangle2D. The last call is what actually
// puts hlms_fog into the pass properties, via preparePassHash.
//
// setLight() is NEVER called. syncToLight() returns immediately without a linked
// light, so the component cannot touch SceneManager::setAmbientLight — our SH
// ambient (and everything the sky/IBL lane computes) stays exactly as it was.
// This is by construction, not by luck: there is no other path from the component
// to the ambient.
//
// What the component gives the shader: hlms_fog + a const buffer with fogDensity
// and the two breakthrough terms, consumed by the stock HlmsPbs pixel shader.
// What it CANNOT give: an authored fog colour (it computes a procedural sky
// colour per vertex) and height fog. Those two ride the pass-buffer extension
// below and media/Hlms/Jahshaka/JahFog_piece_vs_piece_ps.any.
#include "EnginePrivate.h"
#include <CommandBuffer/OgreCbTexture.h>
#include <CommandBuffer/OgreCommandBuffer.h>

namespace jahshaka { namespace engine { namespace detail {

std::map<const Ogre::SceneManager *, FogState> FogHlmsListener::sFogState;   // render thread only
std::map<const Ogre::SceneManager *, float>    FogHlmsListener::sSceneTime;  // render thread only
std::map<const Ogre::SceneManager *, FogHlmsListener::IfdState> FogHlmsListener::sIfdState;  // render thread only
unsigned                                       FogHlmsListener::sLightCountMismatches = 0;
unsigned                                       FogHlmsListener::sMismatchLogged = 0;
std::vector<const Ogre::CompositorShadowNode *> FogHlmsListener::sAssignmentChanged;  // render thread only
std::map<const Ogre::SceneManager *, FogHlmsListener::SkyEnvState>
                                               FogHlmsListener::sSkyEnv;      // render thread only
FogHlmsListener::PassBinds FogHlmsListener::sPass[Ogre::HLMS_MAX];          // render thread only
Ogre::HlmsManager            *FogHlmsListener::sSamplerMgr     = nullptr;     // render thread only
const Ogre::HlmsSamplerblock *FogHlmsListener::sEnvSampler     = nullptr;     // render thread only
const Ogre::HlmsSamplerblock *FogHlmsListener::sGatherSampler  = nullptr;     // render thread only
std::map<const Ogre::SceneManager *, Ogre::TextureGpu *>
                              FogHlmsListener::sProbeGather;                  // render thread only
std::map<const Ogre::SceneManager *, FogHlmsListener::CloudShadowState>
                              FogHlmsListener::sCloudShadow;                  // render thread only
const Ogre::HlmsSamplerblock *FogHlmsListener::sCloudSampler = nullptr;       // render thread only

namespace {
// THE EXTRA PASS TEXTURES, IN THEIR ONE FIXED ORDER (the sky's environment,
// then the gather's irradiance, then the cloud field — CLOUDS-2D-1). Three
// places must agree about it: the count (getNumExtraPassTextures), the
// registers (propertiesMergedPreGenerationStep) and the bindings
// (hlmsTypeChanged). They all walk THIS table, so a fourth slot is one row
// here and one texture in hlmsTypeChanged's list, never three hand-kept
// sequences of arithmetic.
struct ExtraPassSlot {
    const char *property = nullptr;   // the PASS property that claims the slot
    const char *reg = nullptr;        // the register name the piece declares it at
};
constexpr ExtraPassSlot kExtraPassSlots[] = {
    { "jah_env",          "jahEnvCube" },
    { "jah_probe_gather", "jahProbeIrradiance" },
    { "jah_cloud_shadow", "jahCloudField" },
};
constexpr size_t kNumExtraPassSlots = sizeof(kExtraPassSlots) / sizeof(kExtraPassSlots[0]);
/// The slot's property as a hashed IdString, hashed once (these run per
/// renderable hash, not per frame).
const Ogre::IdString &extraSlotProperty(size_t i) {
    static const Ogre::IdString ids[kNumExtraPassSlots] = {
        Ogre::IdString(kExtraPassSlots[0].property),
        Ogre::IdString(kExtraPassSlots[1].property),
        Ogre::IdString(kExtraPassSlots[2].property),
    };
    return ids[i];
}
}   // namespace

FogHlmsListener gFogListener;

// THE LAMP-MAP CACHE'S SELF-CHECK — see the declaration for what it guards.
//
// The two numbers that must agree, both written by Hlms::preparePassHashBase:
//   hlms_num_shadow_map_lights = shadowNode->getNumActiveShadowCastingLights()
//                                + (pssmSplits - 1)          [OgreHlms.cpp:3269]
//   the shadow-map INDEX the generated shader reaches, walked from the node's
//   slot array through the pass's cumulative per-type light counts
//                                                       [OgreHlms.cpp:3640-3695]
// The first comes from a count only buildClosestLightList maintains, the second
// from the array setLightFixedToShadowMap writes — which is why ogre-patch 0025
// makes the second invalidate the first's cache.
// G3-a — THE CONE DIFFUSE COMES BACK UNDER A CASCADE CHAIN (PHOTON_SPEC §13 G3,
// the decided option; audit B1 is the finding).
//
// HlmsPbs sets `vct_disable_diffuse` whenever an irradiance field is bound
// (OgreHlmsPbs.cpp:1846-1850) and the whole cone-diffuse block in
// Vct_piece_ps.any is `@property( !vct_disable_diffuse )`. For ONE volume that
// is right: the field covers the entire lit box, so the cones would be a second
// computation of the same term. For a CHAIN it is wrong — the field rides
// cascade 0 (a 10 m box at the High table) and every pixel from there out to the
// outermost cascade would get specular cones and ambient and NO diffuse bounce
// at all, which is the reason the outer cascades exist, discarded by the field's
// binding.
//
// So under a chain the gate is re-opened here, and the two terms are blended by
// the field's own confidence in our JahIfd piece: inside cascade 0 the field
// wins (and its leak fix with it), outside it the chain's cone bounce does.
//
// WHY THIS IS CACHE-SAFE, which the hook's documentation is strict about ("you
// can only set new properties that are DERIVED from existing properties ... a
// property set from external information will break caches"): the condition is
// exactly that. `irradiance_field` and `vct_num_probes` are both already in the
// merged set — the second is the bound VctLighting's cascade count, written by
// HlmsPbs::preparePassHash (OgreHlmsPbs.cpp:1832-1834) — so two passes with the
// same properties always generate the same shader, and a pass that turns the
// chain on or off changes `vct_num_probes` and therefore the pass hash.
//
// WHY NOT A SOURCE PATCH on HlmsPbs, which would be the other honest answer
// (ledger §325): the decision "does a bound field replace the cone diffuse
// everywhere" belongs to whoever placed the field, and here that is us. Upstream
// has one field over one volume and its rule is right for that case; there is
// no defect to fix and no hook missing. This is the hook.
void FogHlmsListener::propertiesMergedPreGenerationStep(
    Ogre::Hlms *hlms, const Ogre::HlmsCache &, const Ogre::HlmsPropertyVec &,
    const Ogre::PiecesMap *, const Ogre::HlmsPropertyVec &, const Ogre::QueuedRenderable &,
    size_t tid) {
    // THE SKY'S TEXTURE REGISTER (lane SKY-FALLBACK-1). HlmsPbs reserved the
    // slot for us in calculateHashFor — `texUnit += getNumExtraPassTextures()`
    // immediately before it writes `set0_texture_slot_end` — so the register is
    // the last one of set 0, and this is the hook that names it. Upstream's own
    // Terra listener does the identical thing one line at a time
    // (Samples/2.0/Tutorials/Tutorial_Terrain/.../OgreHlmsPbsTerraShadows.cpp).
    //
    // Cache-safe, which this hook's documentation is strict about: the register
    // is derived from `set0_texture_slot_end`, a property already in the merged
    // set, and `jah_env` is a PASS property set in preparePassHash,
    // so the same property set always yields the same shader.
    {
        static const Ogre::IdString kSet0End("set0_texture_slot_end");
        static const Ogre::IdString kShadowCaster("hlms_shadowcaster");
        if (!hlms->_getProperty(tid, kShadowCaster)) {
            bool claimed[kNumExtraPassSlots];
            Ogre::int32 extras = 0;
            for (size_t i = 0; i < kNumExtraPassSlots; ++i) {
                claimed[i] = hlms->_getProperty(tid, extraSlotProperty(i)) != 0;
                if (claimed[i]) ++extras;
            }
            // The extras are the LAST registers of set 0 (HlmsPbs reserved
            // them with `texUnit += getNumExtraPassTextures()` immediately
            // before writing set0_texture_slot_end), in kExtraPassSlots' order.
            Ogre::int32 slot = hlms->_getProperty(tid, kSet0End) - extras;
            if (slot >= 0)
                for (size_t i = 0; i < kNumExtraPassSlots; ++i)
                    if (claimed[i])
                        hlms->_setTextureReg(tid, Ogre::PixelShader, kExtraPassSlots[i].reg, slot++);
        }
    }
    {
        // GA-GLASS (PHOTON-GATHER-1b): A BLENDED FRAGMENT READS NO PROBE. The
        // gather's texel under a fragment is the OPAQUE surface the prepass drew
        // there — a glass, fade or additive fragment in front of it would take
        // that surface's irradiance at full coverage (measured: a Glass slab
        // moved 1.9/255 and a Blend slab 18.6/255 on the gather's toggle,
        // gi.gather_glass). So the renderable's copy of the pass property is
        // withdrawn here, AFTER its texture register was claimed above (the slot
        // stays bound and numbered — the cloud field's register behind it does
        // not move — and simply goes undeclared), and every guard that reads it
        // follows: the piece, the field's cage hook, the declaration, and the
        // cone-diffuse switch below. The blended fragment keeps the field, the
        // cones and the environment it had. Cache-safe: derived from two
        // properties already in the merged set.
        static const Ogre::IdString kProbeGather("jah_probe_gather");
        static const Ogre::IdString kAlphaBlend("hlms_alphablend");
        if (hlms->_getProperty(tid, kProbeGather) && hlms->_getProperty(tid, kAlphaBlend))
            hlms->_setProperty(tid, kProbeGather, 0);
    }
    static const Ogre::IdString kIrradianceField("irradiance_field");
    static const Ogre::IdString kVctNumProbes("vct_num_probes");
    static const Ogre::IdString kVctDisableDiffuse("vct_disable_diffuse");
    {
        // GATHER-0: WHERE THE GATHER ANSWERS, THE CONES MUST NOT ALSO ANSWER.
        // The six cone marches per pixel and the probe's 64 rays estimate the
        // SAME integral; adding them is that integral twice. This is the whole
        // of spec section 3's first row, in its phase-0 form — and it is the
        // switch that makes the measured arms comparable at all. Cache-safe:
        // derived from a pass property already in the merged set.
        static const Ogre::IdString kProbeGather("jah_probe_gather");
        static const Ogre::IdString kShadowCaster("hlms_shadowcaster");
        if (hlms->_getProperty(tid, kProbeGather) && !hlms->_getProperty(tid, kShadowCaster)) {
            hlms->_setProperty(tid, kVctDisableDiffuse, 1);
            return;
        }
    }
    if (!hlms->_getProperty(tid, kIrradianceField)) return;
    if (hlms->_getProperty(tid, kVctNumProbes) <= 1) return;
    hlms->_setProperty(tid, kVctDisableDiffuse, 0);
}

// THE SKY IS THE ENVIRONMENT WHEREVER NO PROBE COVERS THE PIXEL — the host half
// of ogre-patch 0048. The whole mechanism is three listener overrides and one
// float4 of pass data; the shader half is the patch.
//
// THE GATE IS `hlms_enable_cubemaps_auto`, i.e. exactly the passes where the
// env-probe slot holds the probe ARRAY and the sky was therefore evicted. With
// no probe grid nothing here fires, no property is set, no slot is claimed and
// every generated shader is byte-identical to a build without this lane — which
// is what keeps the default scene's selftest hash where it is.
Ogre::uint16 FogHlmsListener::getNumExtraPassTextures(const Ogre::HlmsPropertyVec &properties,
                                                      bool casterPass) const {
    if (casterPass) return 0u;
    // THE EXTRAS AND THEIR ORDER are kExtraPassSlots' (the top of this file):
    // this count, the registers claimed in propertiesMergedPreGenerationStep
    // and the bindings emitted in hlmsTypeChanged all walk that one table.
    Ogre::uint16 n = 0u;
    for (size_t i = 0; i < kNumExtraPassSlots; ++i)
        if (Ogre::Hlms::getProperty(properties, extraSlotProperty(i)) != 0) ++n;
    return n;
}

void FogHlmsListener::hlmsTypeChanged(bool casterPass, Ogre::CommandBuffer *commandBuffer,
                                      const Ogre::HlmsDatablock *datablock, size_t texUnit) {
    // The pair is set together in preparePassHash or not at all: a slot claimed
    // by getNumExtraPassTextures and left unbound is an undefined descriptor,
    // and the two conditions must therefore be the SAME condition. The host is
    // the datablock's creator: its own pass's copy, never another host's.
    if (casterPass || !commandBuffer || !datablock || !datablock->getCreator()) return;
    const PassBinds &pb = sPass[datablock->getCreator()->getType()];
    // kExtraPassSlots' order: the sky's environment, GATHER-0's irradiance,
    // the cloud field. Each pair was set together in preparePassHash with its
    // property, or not at all.
    const struct { Ogre::TextureGpu *tex; const Ogre::HlmsSamplerblock *sampler; } bound[] = {
        { pb.skyCube, pb.skySampler },
        { pb.probeGather, pb.probeGatherSampler },
        { pb.cloudField, pb.cloudSampler },
    };
    static_assert(sizeof(bound) / sizeof(bound[0]) == kNumExtraPassSlots,
                  "one binding per extra pass slot, in kExtraPassSlots' order");
    size_t unit = texUnit;
    for (const auto &b : bound) {
        if (!b.tex || !b.sampler) continue;
        *commandBuffer->addCommand<Ogre::CbTexture>() =
            Ogre::CbTexture(Ogre::uint16(unit), b.tex, b.sampler);
        ++unit;
    }
}

void FogHlmsListener::setSkyEnv(const Ogre::SceneManager *sm, const SkyEnvState &state) {
    if (!sm) return;
    if (!state.cube || (state.gain[0] <= 0.0f && state.gain[1] <= 0.0f && state.gain[2] <= 0.0f)) {
        sSkyEnv.erase(sm);
        return;
    }
    sSkyEnv[sm] = state;
}

FogHlmsListener::SkyEnvState FogHlmsListener::skyEnv(const Ogre::SceneManager *sm) {
    auto it = sSkyEnv.find(sm);
    return it == sSkyEnv.end() ? SkyEnvState() : it->second;
}

// GATHER-0 — the screen-probe gather spike's registration (see the header).
void FogHlmsListener::setProbeGather(const Ogre::SceneManager *sm, Ogre::TextureGpu *irradiance) {
    if (!sm) return;
    if (!irradiance) { sProbeGather.erase(sm); return; }
    sProbeGather[sm] = irradiance;
}
void FogHlmsListener::clearProbeGather() { sProbeGather.clear(); }
Ogre::TextureGpu *FogHlmsListener::probeGather(const Ogre::SceneManager *sm) {
    auto it = sProbeGather.find(sm);
    return it == sProbeGather.end() ? nullptr : it->second;
}

void FogHlmsListener::preparePassHash(const Ogre::CompositorShadowNode *shadowNode, bool casterPass,
                                      bool, Ogre::SceneManager *sceneManager, Ogre::Hlms *hlms) {
    // THE FOG'S COLOUR MODE, first and unconditionally for a colour pass: it is
    // a SHADER property (the media file's @undefpiece of upstream's per-vertex
    // sky colour is gated on it), so it has to be set before any of the early
    // returns below — and it participates in the pass hash, which is what makes
    // flipping the row recompile rather than silently keep the old shader.
    if (hlms && !casterPass && sceneManager && lookup(sceneManager).atmosphere)
        hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_fog_atmo", 1);
    // SURFACE-CACHE phase 2. The capture workspace's five-target G-buffer is
    // written by JahCardCapture_piece_ps.any under this ONE pass property, and
    // this is where it is set — the same hook and the same shape as
    // `jah_fog_atmo` above. It is true only while the surface cache's capture
    // workspace is inside its own `_update()`, so every OTHER pass in the
    // process generates the shader it generated before this lane existed and
    // both selftest hashes are unmoved.
    if (hlms && !casterPass && surfaceCardsCapturing())
        hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_card_capture", 1);
    // THE ENVIRONMENT'S SLOT (PHOTON-ENV-1; first claimed by ogre-patch 0048 for
    // the probe-array pass alone), decided here and read twice afterwards: by
    // getNumExtraPassTextures (through the PROPERTY, on any thread) and by
    // hlmsTypeChanged (through this host's PassBinds, on this one).
    //
    // CLAIMED WHEREVER SOMETHING IN THE PASS READS IT: a bound voxel volume (every
    // cone's escape reads the environment — Vct_piece_ps.any, jah_environment.glsl)
    // or an automatic PCC holding the env-probe slot (its no-probe fallback;
    // `hlms_enable_cubemaps_auto` is HlmsPbs's own pass property, set for a bound
    // PCC that is not itself capturing). Both are set earlier in the same
    // HlmsPbs::preparePassHash call. A scene with neither — or with no cube, or a
    // Sky Light at zero gain (setSkyEnv drops the state) — claims nothing, and its
    // shaders are those of a build without the slot.
    //
    // ITS OWN SAMPLER, acquired ONCE per manager and never per pass (the
    // samplerblock reference count is a uint16 — DOCS/traps/ENGINE.md): the slot
    // exists without a PCC now, so the PCC's block cannot be borrowed.
    PassBinds unused;
    PassBinds &pb = hlms ? sPass[hlms->getType()] : unused;
    pb.skyCube = nullptr;
    pb.skySampler = nullptr;
    if (hlms && !casterPass && sceneManager) {
        static const Ogre::IdString kCubemapsAuto("hlms_enable_cubemaps_auto");
        static const Ogre::IdString kVctNumProbes("vct_num_probes");
        const SkyEnvState sky = skyEnv(sceneManager);
        const bool reader = hlms->_getProperty(Ogre::Hlms::kNoTid, kCubemapsAuto) != 0 ||
                            hlms->_getProperty(Ogre::Hlms::kNoTid, kVctNumProbes) > 0;
        if (sky.cube && reader) {
            const Ogre::HlmsSamplerblock *envSampler =
                acquireSampler(hlms->getHlmsManager(), true);
            if (envSampler) {
                pb.skyCube = sky.cube;
                pb.skySampler = envSampler;
                hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_env", 1);
            }
        }
    }
    // GATHER-0 — THE SCREEN-PROBE GATHER SPIKE (2026-09-21). The same three
    // decisions in one place as the sky's slot above: the PROPERTY (which
    // makes the pixel piece exist and participates in the pass hash), the
    // TEXTURE, and the SAMPLER — set together or not at all.
    //
    // `sProbeGather` is empty in every build that never arms the spike and on
    // every frame of one that has disarmed it, so this is one map lookup on a
    // colour pass and nothing else changes anywhere.
    pb.probeGather = nullptr;
    pb.probeGatherSampler = nullptr;
    if (hlms && !casterPass && sceneManager && !sProbeGather.empty()) {
        Ogre::TextureGpu *gather = probeGather(sceneManager);
        if (gather) {
            // A POINT sampler: the piece reads the texel under the fragment,
            // never between two of them.
            //
            // ACQUIRED ONCE PER MANAGER, NOT ONCE PER PASS, and that is not
            // tidiness: `HlmsManager::getSamplerblock` takes a REFERENCE and
            // `HlmsSamplerblock::mRefCount` is a uint16 (OgreHlmsDatablock.h),
            // so acquiring one per colour pass per frame wraps the counter to
            // zero in about six minutes of drawing and destroys a block that
            // is bound. One reference is taken for the manager's life and
            // given back by releaseSamplers (~OgreEngine, before Root).
            const Ogre::HlmsSamplerblock *gatherSampler =
                acquireSampler(hlms->getHlmsManager(), false);
            // THE PAIR IS SET TOGETHER OR NOT AT ALL: a slot claimed by
            // getNumExtraPassTextures and left unbound is an undefined
            // descriptor, so no sampler means no property either.
            if (gatherSampler) {
                pb.probeGatherSampler = gatherSampler;
                pb.probeGather = gather;
                hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_probe_gather", 1);
            }
        }
    }
    // THE CLOUD LAYER'S GROUND SHADOW (CLOUDS-2D-1) — the same three decisions
    // as the two slots above, set together or not at all. `sCloudShadow` is
    // empty in every scene without a layer, so this is one map test per colour
    // pass and nothing else anywhere.
    pb.cloudField = nullptr;
    pb.cloudSampler = nullptr;
    if (hlms && !casterPass && sceneManager && !sCloudShadow.empty()) {
        const CloudShadowState cloud = cloudShadow(sceneManager);
        if (cloud.field) {
            const Ogre::HlmsSamplerblock *wrap = acquireWrapSampler(hlms->getHlmsManager());
            if (wrap) {
                pb.cloudField = cloud.field;
                pb.cloudSampler = wrap;
                hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_cloud_shadow", 1);
            }
        }
    }
    if (casterPass || !shadowNode || !hlms) return;
    // ONLY WHERE AN ASSIGNMENT CHANGED (clean-2 lane, 2026-09-13). A node can
    // only ENTER the broken state when setLightFixedToShadowMap is called on
    // it, which happens in two places — applyShadowCacheDirties and
    // releaseShadowLamp — and both mark the node. On every other frame this is
    // one empty() test per pass.
    //
    // It used to be gated on `anyCached` alone — true for every node in a
    // scene with a cached lamp, which since E2 made caching automatic is every
    // scene with a point or spot lamp. So the six property reads below, each a
    // LINEAR SCAN of the merged property vector, ran for the view pass, all six
    // probe faces and every planar arm, every frame — exactly what the comment
    // beside them said must not happen.
    if (sAssignmentChanged.empty()) return;
    if (std::find(sAssignmentChanged.begin(), sAssignmentChanged.end(), shadowNode) ==
        sAssignmentChanged.end())
        return;
    // Only a node that holds a CACHED lamp can be in the broken state, and that
    // test is a walk of at most seventeen pointers — the property reads below
    // are linear scans and must not run on every pass of every frame.
    bool anyCached = false;
    const Ogre::LightClosestArray &held = shadowNode->getShadowCastingLights();
    for (size_t i = 0; i < held.size() && !anyCached; ++i) anyCached = held[i].isStatic && held[i].light;
    if (!anyCached) return;
    const auto prop = [hlms](const char *name) {
        return hlms->_getProperty(Ogre::Hlms::kNoTid, Ogre::IdString(name), 0);
    };
    const int declared = prop("hlms_num_shadow_map_lights");
    const int pssm     = prop("hlms_pssm_splits");
    const int dirCast  = prop("hlms_lights_directional");
    const int dirAll   = prop("hlms_lights_directional_non_caster");
    const int spotCum  = prop("hlms_lights_spot");
    const int staticBr = prop("hlms_static_branch_shadow_map_lights");
    int needed = (pssm ? pssm : (dirCast > 0 ? 1 : 0)) + (dirCast > 0 ? dirCast - 1 : 0);
    if (!staticBr) needed += spotCum - dirAll;   // the point+spot casters, cumulative
    if (needed <= declared) return;
    ++sLightCountMismatches;
    if (sMismatchLogged++ < 4u) {
        std::string slots;
        for (size_t i = 0; i < held.size(); ++i) {
            slots += "[" + std::to_string(i) + ":";
            if (!held[i].light) slots += "-";
            else slots += std::string(held[i].isStatic ? "cached " : "dynamic ") +
                          (held[i].light->getType() == Ogre::Light::LT_DIRECTIONAL ? "dir"
                           : held[i].light->getType() == Ogre::Light::LT_POINT ? "point" : "spot");
            slots += "]";
        }
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka shadow cache: a pass hashed against '" +
                shadowNode->getDefinition()->getNameStr() + "' declares " +
                std::to_string(declared) + " shadow maps but its lights index " +
                std::to_string(needed) + " (the node reports " +
                std::to_string(shadowNode->getNumActiveShadowCastingLights()) +
                " active casting lights, slots " + slots +
                "). The generated shader cannot compile — see ogre-patch 0025.",
            Ogre::LML_CRITICAL);
    }
}

void FogHlmsListener::noteShadowAssignmentChanged(const Ogre::CompositorShadowNode *node) {
    if (!node) return;
    if (std::find(sAssignmentChanged.begin(), sAssignmentChanged.end(), node) ==
        sAssignmentChanged.end())
        sAssignmentChanged.push_back(node);
}

void FogHlmsListener::clearShadowAssignmentChanges() {
    // clear(), never a fresh vector: this runs every frame and the capacity is
    // one pointer per shadow-node instance in the process.
    sAssignmentChanged.clear();
}

void FogHlmsListener::registerScene(const Ogre::SceneManager *sm, const FogState &p) {
    sFogState[sm] = p;
}

void FogHlmsListener::unregisterFog(const Ogre::SceneManager *sm) {
    sFogState.erase(sm);
}

void FogHlmsListener::unregisterScene(const Ogre::SceneManager *sm) {
    sFogState.erase(sm);
    // The clock table is keyed by SceneManager POINTER, and Ogre recycles those
    // addresses — a scene destroyed and another created would otherwise inherit
    // a stale time. Cleared here rather than in setFog's disable branch: this
    // one is the scene's teardown (OgreScene.cpp), that one is "fog off".
    sSceneTime.erase(sm);
    sIfdState.erase(sm);
    // ...and the sky cube, for the same recycled-pointer reason. The texture it
    // names dies with the scene.
    sSkyEnv.erase(sm);
    // Every host's pass copy, whole (its textures may name this scene's).
    for (PassBinds &pb : sPass) pb = PassBinds();
    sCloudShadow.erase(sm);
}

void FogHlmsListener::setCloudShadow(const Ogre::SceneManager *sm, const CloudShadowState &state) {
    if (!sm) return;
    if (!state.field || state.strength <= 0.0f) { sCloudShadow.erase(sm); return; }
    sCloudShadow[sm] = state;
}

FogHlmsListener::CloudShadowState FogHlmsListener::cloudShadow(const Ogre::SceneManager *sm) {
    auto it = sCloudShadow.find(sm);
    return it == sCloudShadow.end() ? CloudShadowState() : it->second;
}

void FogHlmsListener::setSceneTime(const Ogre::SceneManager *sm, float seconds) {
    sSceneTime[sm] = seconds;
}

float FogHlmsListener::sceneTime(const Ogre::SceneManager *sm) {
    const auto it = sSceneTime.find(sm);
    return it == sSceneTime.end() ? 0.0f : it->second;
}

void FogHlmsListener::setIfdState(const Ogre::SceneManager *sm, const IfdState &state) {
    sIfdState[sm] = state;
}

FogHlmsListener::IfdState FogHlmsListener::ifdState(const Ogre::SceneManager *sm) {
    const auto it = sIfdState.find(sm);
    if (it != sIfdState.end()) return it->second;
    // Not zeros: a scene that has a field bound but never pushed state
    // (impossible today — the arm writes it before it binds) must still render
    // at the calibrated brightness rather than at upstream's raw one. GiParams
    // is the single source of that number. The probe counts stay 0, which
    // JahIfd reads as "no counts" and leaves the cage unclamped (upstream's
    // behaviour).
    IfdState fallback;
    const GiParams defaults;
    fallback.intensity = defaults.ddgiIntensity;
    return fallback;
}

FogState FogHlmsListener::lookup(const Ogre::SceneManager *sm) {
    FogState p;
    const auto it = sFogState.find(sm);
    if (it != sFogState.end()) p = it->second;
    return p;
}

// ONE REFERENCE PER MANAGER, TAKEN ON FIRST USE AND GIVEN BACK BY
// releaseSamplers. A manager other than the one holding the references (which
// only a missed release could leave) is answered by releasing the old pair
// first rather than by trusting a pointer compare.
const Ogre::HlmsSamplerblock *FogHlmsListener::acquireWrapSampler(Ogre::HlmsManager *mgr) {
    if (!mgr) return nullptr;
    if (sSamplerMgr && sSamplerMgr != mgr) releaseSamplers();
    sSamplerMgr = mgr;
    if (!sCloudSampler) {
        Ogre::HlmsSamplerblock ref;
        ref.setFiltering(Ogre::TFO_TRILINEAR);
        ref.setAddressingMode(Ogre::TAM_WRAP);
        sCloudSampler = mgr->getSamplerblock(ref);
    }
    return sCloudSampler;
}

const Ogre::HlmsSamplerblock *FogHlmsListener::acquireSampler(Ogre::HlmsManager *mgr,
                                                              bool trilinear) {
    if (!mgr) return nullptr;
    if (sSamplerMgr && sSamplerMgr != mgr) releaseSamplers();
    sSamplerMgr = mgr;
    const Ogre::HlmsSamplerblock *&slot = trilinear ? sEnvSampler : sGatherSampler;
    if (!slot) {
        Ogre::HlmsSamplerblock ref;
        ref.setFiltering(trilinear ? Ogre::TFO_TRILINEAR : Ogre::TFO_NONE);
        ref.setAddressingMode(Ogre::TAM_CLAMP);
        slot = mgr->getSamplerblock(ref);
    }
    return slot;
}

void FogHlmsListener::releaseSamplers() {
    if (sSamplerMgr) {
        if (sEnvSampler)    sSamplerMgr->destroySamplerblock(sEnvSampler);
        if (sGatherSampler) sSamplerMgr->destroySamplerblock(sGatherSampler);
        if (sCloudSampler)  sSamplerMgr->destroySamplerblock(sCloudSampler);
    }
    sSamplerMgr = nullptr;
    sEnvSampler = sGatherSampler = sCloudSampler = nullptr;
    for (PassBinds &pb : sPass) pb = PassBinds();
}

Ogre::uint32 FogHlmsListener::getPassBufferSize(const Ogre::CompositorShadowNode *, bool, bool,
                                                Ogre::SceneManager *) const {
    // Constant, fog on or off, caster or not: the shader's struct may be SHORTER
    // than the buffer (it is, whenever fog is off), never longer. Four for the
    // shader clock (HLMS_ADOPTION P5) — declared only by materials that carry a
    // generated piece — and four for the DDGI block (GI_UNIFIED P1 and the
    // ambient fix), declared only while an IrradianceField is bound. Both
    // written always, because this hook cannot know which materials the pass
    // will draw, and sixteen unconditional bytes are cheaper than a size that
    // varies per pass.
    // Plus jahEnv (PHOTON-ENV-1): the environment cube's gain per channel and
    // mip count, declared only by a pass that claimed the environment's slot.
    //
    // Nothing here compensates for the irradiance field's block any more: its
    // own getConstBufferSize() under-reported by one float4 until ogre-patch
    // 0050, and this listener used to reserve four extra floats on a
    // field-bound non-caster pass and then deliberately SKIP them, so that the
    // field's 24-float write and HlmsPbs's 20-float pointer advance could not
    // collide with our first float4. That correction depended on HlmsPbs
    // filling the field's block BEFORE calling this listener; the size is
    // simply right now.
    // Plus the cloud layer's ground shadow (CLOUDS-2D-1): five float4, the
    // last members, declared only by a pass that claimed the cloud field.
    return 40u * sizeof(float);
}

float *FogHlmsListener::preparePassBuffer(const Ogre::CompositorShadowNode *, bool, bool,
                                          Ogre::SceneManager *sceneManager, float *passBufferPtr) {
    const FogState p = lookup(sceneManager);
    // The height layer integrates from the CAMERA's altitude, so the shader needs
    // it; this hook runs inside HlmsPbs::preparePassBuffer, where the camera of
    // the pass being built is current.
    float cameraY = 0.0f;
    if (const Ogre::Camera *cam = sceneManager->getCamerasInProgress().renderingCamera)
        cameraY = cam->getDerivedPosition().y;
    *passBufferPtr++ = p.r;
    *passBufferPtr++ = p.g;
    *passBufferPtr++ = p.b;
    *passBufferPtr++ = p.heightDensity;
    *passBufferPtr++ = p.heightFalloff;
    *passBufferPtr++ = p.heightLevel;
    *passBufferPtr++ = cameraY;
    *passBufferPtr++ = 0.0f;
    // The clock. Four floats so the struct stays 16-byte aligned like every
    // other member of a std140 buffer; x is seconds, yzw are reserved for the
    // things a generated piece will want next (frame index, delta, a seed).
    *passBufferPtr++ = sceneTime(sceneManager);
    *passBufferPtr++ = 0.0f;
    *passBufferPtr++ = 0.0f;
    *passBufferPtr++ = 0.0f;
    // The DDGI block, same four-float alignment rule. Read by
    // media/Hlms/Jahshaka/JahIfd_piece_ps.any, which only exists in the
    // generated shader while an IrradianceField is bound: x scales the field's
    // irradiance (the sky its probes see included, PHOTON-ENV-1), y is the
    // field's window offset, packed (PHOTON-WRITER-1's scroll: the reader's
    // modulo), zw are the field's Y and Z probe counts, which upstream's own
    // IrradianceField block does not carry and the cage clamp needs.
    const IfdState ifd = ifdState(sceneManager);
    *passBufferPtr++ = ifd.intensity;
    *passBufferPtr++ = ifd.windowOffsetPacked;     // the field's window (PHOTON-WRITER-1)
    *passBufferPtr++ = ifd.numProbesY;
    *passBufferPtr++ = ifd.numProbesZ;
    // jahEnv (PHOTON-ENV-1): rgb = the environment light's gain per channel on
    // the cube, w = the cube's own mip count. Written unconditionally like every
    // field above — the shader declares it only when it claimed the slot, and a
    // buffer longer than the struct is fine (a struct longer than the buffer is
    // not).
    //
    // The gain does NOT ride passBuf.ambientUpperHemi.w: that pass scale is one
    // channel, and it is pinned to 1.0 while a PCC is bound because it also
    // multiplies the probe samples (envmapScaleForPass's note).
    const SkyEnvState sky = skyEnv(sceneManager);
    *passBufferPtr++ = sky.cube ? sky.gain[0] : 0.0f;
    *passBufferPtr++ = sky.cube ? sky.gain[1] : 0.0f;
    *passBufferPtr++ = sky.cube ? sky.gain[2] : 0.0f;
    *passBufferPtr++ = sky.cube ? sky.numMipmaps : 1.0f;
    // THE CLOUD LAYER'S GROUND SHADOW (CLOUDS-2D-1): the pass camera's
    // view-to-world rows (the pixel shader holds only the view-space position),
    // then the field's mapping and the sun's throw. Zeros without a layer —
    // no shader of such a pass declares them.
    {
        const CloudShadowState cloud =
            sCloudShadow.empty() ? CloudShadowState() : cloudShadow(sceneManager);
        const Ogre::Camera *cam = sceneManager->getCamerasInProgress().renderingCamera;
        if (cloud.field && cam) {
            // The same view matrix HlmsPbs::preparePassBuffer wrote into
            // passBuf.view for this pass (OgreHlmsPbs.cpp: getVrViewMatrix(0)),
            // inverted: `inPs.pos` is in THAT space.
            const Ogre::Matrix4 inv = cam->getVrViewMatrix(0).inverseAffine();
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) *passBufferPtr++ = float(inv[r][c]);
            *passBufferPtr++ = cloud.invTile;
            *passBufferPtr++ = cloud.strength;
            *passBufferPtr++ = cloud.scroll[0];
            *passBufferPtr++ = cloud.scroll[1];
            *passBufferPtr++ = cloud.sunThrow[0];
            *passBufferPtr++ = cloud.sunThrow[1];
            *passBufferPtr++ = cloud.altitude;
            *passBufferPtr++ = cloud.invMuSun;
        } else {
            for (int i = 0; i < 20; ++i) *passBufferPtr++ = 0.0f;
        }
    }
    return passBufferPtr;
}

void OgreScene::ensureAtmosphere() {
    if (mAtmosphere) return;
    Ogre::VaoManager *vao = mRoot->getRenderSystem()->getVaoManager();
    if (!vao) return;
    // The constructor loads the "Ogre/Atmo/NprSky" material and THROWS when the
    // Atmosphere media is missing, so it runs inside the guard like every other
    // Ogre call here: a scene with no fog is the failure mode, never an exception
    // crossing the boundary. It can only run after registerCommonMaterials() (and
    // therefore after the first render window and Hlms registration), which every
    // caller satisfies — scenes exist only after Engine::createView().
    JAH_TRY {
        mAtmosphere = new Ogre::AtmosphereNpr(vao);
        // The quad needs two things done to it that the component cannot know
        // about (render queue 0 instead of 212, kVisibleBit instead of the
        // default flags — tuneAtmosphereRenderable says why), and it names it
        // since ogre-patch 0054. It used to be found by diffing the
        // SceneManager's Rectangle2D set across the setSky call.
        mAtmosphere->setSky(mSceneMgr, true);      // creates the sky quad, registers
        mAtmoQuad = mAtmosphere->getSky(mSceneMgr);
        tuneAtmosphereRenderable();
        // The state the two customers left behind decides what happens next
        // (ONE component, two customers — syncAtmosphere's header). A first
        // creation from setFog leaves the quad hidden and the component
        // registered; from the analytic sky it leaves both on.
        syncAtmosphere();
    } JAH_CATCH(mError, );
}

void OgreScene::destroyAtmosphere() {
    if (!mAtmosphere) return;
    mAtmoQuad = nullptr;   // the component destroys it in its own destructor
    // ~AtmosphereNpr un-registers itself from every SceneManager it knows and
    // destroys their Rectangle2Ds — which is why this must precede the manager.
    JAH_TRY {
        delete mAtmosphere;
    } JAH_CATCH(mError, );
    mAtmosphere = nullptr;
}

void OgreScene::setFog(const FogDesc &desc) {
    // THE PROBE CACHE'S FOG INPUT (ENGINE_CACHE_POLICY_SPEC P7): the probe
    // faces are fogged PBS renders. Compared by value — the host re-pushes fog
    // on every page return, and an unchanged push must cost no re-capture.
    {
        const FogDesc &o = mLastFogDesc;
        const bool same = mFogDescKnown && o.enabled == desc.enabled &&
            (!desc.enabled ||
             (o.colour.r == desc.colour.r && o.colour.g == desc.colour.g &&
              o.colour.b == desc.colour.b && o.density == desc.density &&
              o.heightDensity == desc.heightDensity && o.heightFalloff == desc.heightFalloff &&
              o.heightLevel == desc.heightLevel &&
              o.breakMinBrightness == desc.breakMinBrightness &&
              o.breakFalloff == desc.breakFalloff));
        mLastFogDesc = desc;
        mFogDescKnown = true;
        if (!same) staleProbeGrid(GiStaleReason::Fog);   // a no-op before a grid exists
    }
    if (!desc.enabled) {
        // Bit-exact off: no atmosphere means no hlms_fog property, which means the
        // fog code is not compiled into the shader at all. Every offscreen pixel
        // suite depends on this.
        //
        // UNLESS THE ANALYTIC SKY IS BOUND (SKY-GPU): the same component draws
        // it, and its registration is what makes the quad update at all, so the
        // fog cannot take it down. The fog block then stays in the shader with
        // fogDensity 0 — an exact identity, see applySkyAtmosphere (3).
        mAtmoFogOn = false;
        if (mAtmoSkyOn) {
            syncAtmosphere();
            if (mAtmosphere) JAH_TRY {
                Ogre::AtmosphereNpr::Preset preset = mAtmosphere->getPreset();
                preset.fogDensity = 0.0f;
                mAtmosphere->setPreset(preset);
                ++mAtmoPresetGeneration;   // atmosphereSunTint's memo is keyed on this
            } JAH_CATCH(mError, );
        } else {
            destroyAtmosphere();
        }
        FogHlmsListener::unregisterFog(mSceneMgr);
        return;
    }
    mAtmoFogOn = true;
    ensureAtmosphere();
    if (!mAtmosphere) return;   // media missing: the scene renders unfogged, mError says why
    syncAtmosphere();

    JAH_TRY {
        Ogre::AtmosphereNpr::Preset preset = mAtmosphere->getPreset();
        preset.fogDensity            = std::max(desc.density, 0.0f);
        preset.fogBreakMinBrightness = std::max(desc.breakMinBrightness, 0.0f);
        preset.fogBreakFalloff       = std::max(desc.breakFalloff, 0.0f);
        // Everything else in the preset drives the sky and the (unlinked) sun; the
        // hidden quad and the absent light make those values unobservable.
        mAtmosphere->setPreset(preset);
        ++mAtmoPresetGeneration;   // atmosphereSunTint's memo is keyed on this

        pushFogState();
    } JAH_CATCH(mError, );
}

// THE FOG STATE THE SHADER READS, derived from the LAST description the host
// pushed — and re-derived whenever the SKY changes, which is the point of it
// being a function.
//
// `atmosphere` (the aerial-perspective mode) is only meaningful while an
// ANALYTIC sky is drawn: the colour it asks for is that sky's own scattering,
// and with any other sky bound the component's model would be evaluated for a
// sky nobody can see. It used to be decided once, at setFog time, and then
// OUTLIVED the sky — pick Realistic, turn Aerial on, switch to a photograph,
// and the fog went on being coloured by an atmosphere that was no longer
// drawn. Now syncAtmosphere calls this on every sky change, so the mode is a
// function of the state rather than of the order the host pushed things in.
void OgreScene::pushFogState() {
    if (!mAtmoFogOn || !mFogDescKnown) return;
    const FogDesc &desc = mLastFogDesc;
    FogState s;
    s.r = desc.colour.r; s.g = desc.colour.g; s.b = desc.colour.b;
    s.heightDensity = std::max(desc.heightDensity, 0.0f);
    s.heightFalloff = desc.heightFalloff;
    s.heightLevel   = desc.heightLevel;
    s.atmosphere    = desc.atmosphereColour && mAtmoSkyOn;
    FogHlmsListener::registerScene(mSceneMgr, s);   // read by preparePassBuffer
}

}}}  // namespace jahshaka::engine::detail
