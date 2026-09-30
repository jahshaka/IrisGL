// Fog: Ogre's fog block (HlmsPbs, under `hlms_fog`) fed by OUR atmosphere
// component, wired to Jahshaka's authored colour and extended with a height
// layer — and, under the planet's atmosphere, the air's own aerial perspective.
//
// THE COMPONENT (Atmosphere.h, JahAtmosphere) is an Ogre::AtmosphereComponent:
// registered on a SceneManager (`_setAtmosphere`) it sets `hlms_fog` in every
// colour pass and binds the const buffer upstream's block reads its density
// and brightness breakthrough from. It serves two customers — the sky, when it
// is the planet's atmosphere, and the World fog under any sky — and is
// registered while either wants it (OgreScene::syncAtmosphere).
//
// What upstream's block CANNOT give: an authored fog colour and height fog.
// Those ride the pass-buffer extension below and
// media/Hlms/Jahshaka/JahFog_piece_vs_piece_ps.any.
//
// UNDER THE PLANET'S ATMOSPHERE THE AIR IS A MEDIUM. Every lit pixel is seen
// through the aerial-perspective table at its own direction and distance (the
// light the air scatters in, the transmittance), whether or not the World fog
// is on; the World fog and its height layer compose on top, fogging towards
// the sky's own radiance just above the horizon at the pixel's azimuth. The
// block's own density is 0 there (an identity) and our piece does all of it.
// The authored colour is only for the other skies.
#include "EnginePrivate.h"
#include "Atmosphere.h"
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
std::map<const Ogre::SceneManager *, FogHlmsListener::SunContactBind>
                              FogHlmsListener::sSunContact;                   // render thread only
std::map<const Ogre::SceneManager *, int>
                              FogHlmsListener::sPhotonIsolation;              // render thread only
const Ogre::HlmsSamplerblock *FogHlmsListener::sCloudSampler = nullptr;       // render thread only
std::map<const Ogre::SceneManager *, FogHlmsListener::AtmoBind>
                              FogHlmsListener::sAtmo;                         // render thread only

namespace {
// THE EXTRA PASS TEXTURES, IN THEIR ONE FIXED ORDER (the sky's environment,
// then the gather's irradiance, then the cloud field — CLOUDS-2D-1 — then the
// sun contact visibility — PHOTON-RAYS-1 — then the atmosphere's
// aerial-perspective volume — SKY-ATMOSPHERE-1). Three
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
    { "jah_sun_contact",  "jahSunVis" },
    { "jah_atmo_ap",      "jahAtmoAerial" },
};
constexpr size_t kNumExtraPassSlots = sizeof(kExtraPassSlots) / sizeof(kExtraPassSlots[0]);
// THE PASS-TEXTURE TABLE HAS A HARD SIZE, AND A PASS OVER IT IS NOT SURVIVABLE
// (SKY-ATMOSPHERE-1's gate: world_sky.clouds_2d asked for 65 of 64 —
// VulkanRootLayout threw "set 0 needs 65 texture slots; the global binding table
// holds 64" and the process died with SIGSEGV). The table is the fork's
// NUM_BIND_TEXTURES (RenderSystems/Vulkan/include/OgreVulkanGlobalBindingTable.h,
// 64); the pin's own share of the fullest pass this engine builds — Epic, the
// gather, the sun contact, the cloud field and the environment claimed — is 59,
// MEASURED as that crash's 65 less its six extras. So the extras may never
// exceed five, and adding a sixth is a BUILD error here rather than a crash in
// one arm of one pool. Five today: the fullest pass is at 64 of 64 with every
// extra claimed; the atmosphere's volume is claimed only where it is read
// (aerialScale > 0 or the World fog), so a default scene's fullest pass is 63.
constexpr size_t kPassTextureTable = 64u;
constexpr size_t kPinPassTexturesFullest = 59u;
static_assert(kPinPassTexturesFullest + kNumExtraPassSlots <= kPassTextureTable,
              "the PBS pass-texture table overflows: the pin's fullest pass plus kExtraPassSlots "
              "exceeds the fork's 64 slots — fold an extra into an existing texture first");
/// The slot's property as a hashed IdString, hashed once (these run per
/// renderable hash, not per frame).
const Ogre::IdString &extraSlotProperty(size_t i) {
    static const Ogre::IdString ids[kNumExtraPassSlots] = {
        Ogre::IdString(kExtraPassSlots[0].property),
        Ogre::IdString(kExtraPassSlots[1].property),
        Ogre::IdString(kExtraPassSlots[2].property),
        Ogre::IdString(kExtraPassSlots[3].property),
        Ogre::IdString(kExtraPassSlots[4].property),
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
// from the array setLightFixedToShadowMap writes — which is why fork 6130df9d1 (was 0025)
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
    // A BLENDED FRAGMENT IS NOT THE SURFACE THE PREPASS DREW, and three pass
    // properties describe that surface (PHOTON-GATHER-1b GA-GLASS; 1d F2). All
    // three are withdrawn per renderable here, AFTER HlmsPbs numbered the texture
    // registers (notifyPropertiesMergedPreGenerationStep runs before this hook):
    // the slots stay bound and numbered — nothing behind them moves — and simply
    // go undeclared, and every guard that reads a property follows it.
    // Cache-safe: derived from properties already in the merged set.
    static const Ogre::IdString kProbeGather("jah_probe_gather");
    static const Ogre::IdString kEnvDiffuseOnly("jah_env_diffuse_only");
    static const Ogre::IdString kShadowCaster("hlms_shadowcaster");
    const bool blended = hlms->_getProperty(tid, Ogre::IdString("hlms_alphablend")) != 0 &&
                         !hlms->_getProperty(tid, kShadowCaster);
    if (blended) {
        // F2 — THE PREPASS: `hlms_use_prepass` is a PASS property, and under it
        // 800.PixelShader takes the fragment's normal, roughness and directional
        // SHADOW from the G-buffer at iFragCoord — the OPAQUE surface's behind
        // it (measured: a Fade slab 25/255 dark, shading in its own cast shadow;
        // spikes/photon-gather-1b/AUDIT.md F2). Withdrawn, the blended fragment
        // shades from its own interpolants and its own shadow-map lookup, as it
        // does in a pass with no prepass at all. `hlms_use_ssr` goes with it: the
        // SSR texel under the fragment is the opaque surface's reflection. THE
        // OTHER HALF is in the prepass itself: a blended fragment writes nothing
        // there (JahProbeGather_piece_ps.any's discard), so the G-buffer and the
        // depth the shading pass tests against are the opaque surfaces' alone —
        // measured on gi.gather_glass's F2 arm: 31.9/255 dark with neither half,
        // 7.6 with this one alone, 24.1 with the discard alone, 0.0 with both.
        static const Ogre::IdString kUsePrePass("hlms_use_prepass");
        static const Ogre::IdString kUseSsr("hlms_use_ssr");
        if (hlms->_getProperty(tid, kUsePrePass)) {
            hlms->_setProperty(tid, kUsePrePass, 0);
            hlms->_setProperty(tid, kUseSsr, 0);
        }
        // GA-GLASS — THE GATHER: the probe texel under a blended fragment is the
        // opaque surface's irradiance (measured: a Glass slab 1.9/255, a Blend
        // slab 18.6/255 on the gather's toggle, gi.gather_glass). Withdrawn — and
        // THE TRANSLUCENT SURFACE'S DIFFUSE GI IS DECIDED (PHOTON-GATHER-1d): the
        // ENVIRONMENT TERM ONLY — ONE-ENV's irradiance at the fragment's own
        // normal, `jah_env_diffuse_only` (JahProbeGather_piece_ps.any). A blended
        // fragment reads no gather (it has no probe of its own: a probe sits on
        // the surface the prepass drew), no cones (they are compiled out on a
        // gather tier, below) and no field cage (JahIfd_piece_ps.any declines it
        // under the same property): what lights a translucent surface from the
        // scene is its direct light, what it transmits is the refraction's.
        if (hlms->_getProperty(tid, kProbeGather)) {
            hlms->_setProperty(tid, kProbeGather, 0);
            hlms->_setProperty(tid, kEnvDiffuseOnly, 1);
        }
    }
    static const Ogre::IdString kIrradianceField("irradiance_field");
    static const Ogre::IdString kVctNumProbes("vct_num_probes");
    static const Ogre::IdString kVctDisableDiffuse("vct_disable_diffuse");
    {
        // WHERE THE GATHER ANSWERS, THE CONES DO NOT (GATHER-0; the tier's fact
        // since PHOTON-GATHER-1d: a pass gathers exactly on a gather tier). The
        // six cone marches per pixel and the probe's 64 rays estimate the SAME
        // integral; adding them is that integral twice. So on a gather pass the
        // cone diffuse is compiled out of every renderable — the gathering ones
        // and the translucent ones above alike — and the cones' diffuse remains
        // Low's alone. Cache-safe: derived from the properties just set.
        if ((hlms->_getProperty(tid, kProbeGather) || hlms->_getProperty(tid, kEnvDiffuseOnly)) &&
            !hlms->_getProperty(tid, kShadowCaster)) {
            hlms->_setProperty(tid, kVctDisableDiffuse, 1);
            return;
        }
    }
    if (!hlms->_getProperty(tid, kIrradianceField)) return;
    if (hlms->_getProperty(tid, kVctNumProbes) <= 1) return;
    hlms->_setProperty(tid, kVctDisableDiffuse, 0);
}

// THE SKY IS THE ENVIRONMENT WHEREVER NO PROBE COVERS THE PIXEL — the host half
// of fork 4d5fbef16+8f09c0cd4 (was 0048). The whole mechanism is three listener overrides and one
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
    // the cloud field, the sun contact visibility, the atmosphere's volume.
    // Each pair was set together in preparePassHash with its property, or not
    // at all.
    const struct { Ogre::TextureGpu *tex; const Ogre::HlmsSamplerblock *sampler; } bound[] = {
        { pb.skyCube, pb.skySampler },
        { pb.probeGather, pb.probeGatherSampler },
        { pb.cloudField, pb.cloudSampler },
        { pb.sunVis, pb.sunVisSampler },
        { pb.atmoAerial, pb.atmoSampler },
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

// PHOTON-RAYS-1 — the sun contact job's registration (see the header).
void FogHlmsListener::setSunContact(const Ogre::SceneManager *sm, Ogre::TextureGpu *visibility,
                                    unsigned divisor) {
    if (!sm) return;
    if (!visibility) { sSunContact.erase(sm); return; }
    SunContactBind b;
    b.tex = visibility;
    b.divisor = divisor ? divisor : 1u;
    sSunContact[sm] = b;
}
void FogHlmsListener::clearSunContact() { sSunContact.clear(); }
// PHOTON-VIEW-1 — the view's isolation, pass-scoped like the gather's registration.
void FogHlmsListener::setPhotonIsolation(const Ogre::SceneManager *sm, int mode) {
    if (!sm) return;
    if (mode <= 0) { sPhotonIsolation.erase(sm); return; }
    sPhotonIsolation[sm] = mode;
}
Ogre::TextureGpu *FogHlmsListener::probeGather(const Ogre::SceneManager *sm) {
    auto it = sProbeGather.find(sm);
    return it == sProbeGather.end() ? nullptr : it->second;
}

void FogHlmsListener::preparePassHash(const Ogre::CompositorShadowNode *shadowNode, bool casterPass,
                                      bool, Ogre::SceneManager *sceneManager, Ogre::Hlms *hlms) {
    PassBinds unused;
    PassBinds &pb = hlms ? sPass[hlms->getType()] : unused;
    // THE PLANET'S ATMOSPHERE'S VOLUME (SKY-ATMOSPHERE-1), first and
    // unconditionally for a colour pass: its PROPERTY is also the
    // fog's colour mode (the media file's air and its per-pixel World fog are
    // gated on them, and upstream's per-vertex colour is redefined away), so it
    // has to be set before any of the early returns below — and it takes part
    // in the pass hash, which is what makes switching the sky recompile rather
    // than silently keep the old shader. The textures, the sampler and the
    // properties are set together or not at all (an unbound claimed slot is an
    // undefined descriptor).
    pb.atmoAerial = nullptr;
    pb.atmoSampler = nullptr;
    if (hlms && !casterPass && sceneManager && !sAtmo.empty() && lookup(sceneManager).atmosphere) {
        auto it = sAtmo.find(sceneManager);
        if (it != sAtmo.end() && it->second.aerial) {
            const Ogre::HlmsSamplerblock *linear = acquireSampler(hlms->getHlmsManager(), true);
            if (linear) {
                pb.atmoAerial = it->second.aerial;
                pb.atmoSampler = linear;
                hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_atmo_ap", 1);
            }
        }
    }
    // SURFACE-CACHE phase 2. The capture workspace's five-target G-buffer is
    // written by JahCardCapture_piece_ps.any under this ONE pass property, and
    // this is where it is set — the same hook and the same shape as the
    // atmosphere's properties above. It is true only while the surface cache's capture
    // workspace is inside its own `_update()`, so every OTHER pass in the
    // process generates the shader it generated before this lane existed and
    // both selftest hashes are unmoved.
    if (hlms && !casterPass && surfaceCardsCapturing())
        hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_card_capture", 1);
    // THE ENVIRONMENT'S SLOT (PHOTON-ENV-1; first claimed by fork 4d5fbef16+8f09c0cd4 (was 0048) for
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
    // HARD SUN CONTACT SHADOWS (PHOTON-RAYS-1) — the gather's three decisions
    // again, set together or not at all, and PASS-SCOPED the same way (the ray
    // tier registers the texture in front of the PrePassUse pass and takes it
    // away when that pass ends), so the capture, probe and card passes of the
    // same SceneManager never see it. The property's VALUE is the texel
    // divisor the piece reads the texture at. `sSunContact` is empty in every
    // scene with the row off: one empty() test per colour pass.
    pb.sunVis = nullptr;
    pb.sunVisSampler = nullptr;
    if (hlms && !casterPass && sceneManager && !sSunContact.empty()) {
        auto it = sSunContact.find(sceneManager);
        if (it != sSunContact.end() && it->second.tex) {
            // The gather's POINT sampler, borrowed (one reference per manager —
            // the uint16 refcount rule above); the piece reads with a texel
            // fetch and never filters.
            const Ogre::HlmsSamplerblock *point = acquireSampler(hlms->getHlmsManager(), false);
            if (point) {
                pb.sunVis = it->second.tex;
                pb.sunVisSampler = point;
                hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_sun_contact",
                                   Ogre::int32(it->second.divisor));
            }
        }
    }
    // THE PHOTON VIEW'S ISOLATION (PHOTON-VIEW-1): a pass property whose VALUE is
    // the mode (1 the indirect diffuse alone, 2 the specular environment alone),
    // read by JahPhotonView_piece_ps.any. Empty in every scene whose view is not
    // Diffuse or Reflections, and set only for the length of the one pass that
    // shades the picture: one empty() test per colour pass otherwise, and every
    // generated shader is the one it was.
    if (hlms && !casterPass && sceneManager && !sPhotonIsolation.empty()) {
        auto it = sPhotonIsolation.find(sceneManager);
        if (it != sPhotonIsolation.end() && it->second > 0)
            hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_photon_view", Ogre::int32(it->second));
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
                "). The generated shader cannot compile — see fork 6130df9d1 (was 0025).",
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
    sSunContact.erase(sm);
    sPhotonIsolation.erase(sm);
    sAtmo.erase(sm);
}

void FogHlmsListener::setAtmosphere(const Ogre::SceneManager *sm, const AtmoBind &bind) {
    if (!sm) return;
    if (!bind.aerial) { sAtmo.erase(sm); return; }
    sAtmo[sm] = bind;
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
    return IfdState();
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
    // own getConstBufferSize() under-reported by one float4 until fork ae2ed529f+822d538f5
    // (was 0050), and this listener used to reserve four extra floats on a
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
    *passBufferPtr++ = p.distanceDensity;   // jahFogHeight.w (FOG-ATMO-1)
    // The clock. Four floats so the struct stays 16-byte aligned like every
    // other member of a std140 buffer; x is seconds, yzw are reserved for the
    // things a generated piece will want next (frame index, delta, a seed).
    *passBufferPtr++ = sceneTime(sceneManager);
    *passBufferPtr++ = 0.0f;
    *passBufferPtr++ = 0.0f;
    *passBufferPtr++ = 0.0f;
    // The DDGI block, same four-float alignment rule. Read by
    // media/Hlms/Jahshaka/JahIfd_piece_ps.any, which only exists in the
    // generated shader while an IrradianceField is bound — a float3 (the
    // intensity dial's float is deleted, PHOTON-GATHER-1d): x is the field's
    // window offset, packed (PHOTON-WRITER-1's scroll: the reader's modulo), yz
    // are the field's Y and Z probe counts, which upstream's own
    // IrradianceField block does not carry and the cage clamp needs. Then the
    // std140 pad before jahEnv's 16-byte alignment.
    const IfdState ifd = ifdState(sceneManager);
    *passBufferPtr++ = ifd.windowOffsetPacked;     // the field's window (PHOTON-WRITER-1)
    *passBufferPtr++ = ifd.numProbesY;
    *passBufferPtr++ = ifd.numProbesZ;
    *passBufferPtr++ = 0.0f;                       // std140 pad
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
    if (!mRoot->getRenderSystem()->getVaoManager()) return;
    // The component loads its sky material and THROWS when the media is not
    // staged, so it runs inside the guard like every other Ogre call here: a
    // scene with no fog and no atmosphere is the failure mode, never an
    // exception crossing the boundary. Scenes exist only after
    // Engine::createView(), so the window and the Hlms are up.
    JAH_TRY {
        mAtmosphere = new JahAtmosphere(mRoot, mSceneMgr, kVisibleBit);
        // The state the two customers left behind decides what happens next
        // (ONE component, two customers — syncAtmosphere's header).
        syncAtmosphere();
    } JAH_CATCH(mError, );
}

void OgreScene::destroyAtmosphere() {
    if (!mAtmosphere) return;
    FogHlmsListener::setAtmosphere(mSceneMgr, FogHlmsListener::AtmoBind());
    // The component unregisters itself and destroys its quad through the
    // SceneManager — which is why this must precede the manager.
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
        // Bit-exact off: no component registered means no hlms_fog property,
        // which means the fog code is not compiled into the shader at all.
        // Every offscreen pixel suite depends on this.
        //
        // UNLESS THE ATMOSPHERE IS THE SKY: the same component draws it, and
        // under it the AIR is still a medium — syncAtmosphere -> pushFogState
        // leaves the air's aerial perspective on, without the World fog's
        // density, height layer or breakthrough.
        mAtmoFogOn = false;
        if (mAtmoSkyOn || mHeightFogOn) {
            syncAtmosphere();
        } else {
            destroyAtmosphere();
            FogHlmsListener::unregisterFog(mSceneMgr);
        }
        return;
    }
    mAtmoFogOn = true;
    ensureAtmosphere();
    if (!mAtmosphere) return;   // media missing: the scene renders unfogged, mError says why
    syncAtmosphere();           // ...which ends in pushFogState: the component's fog block and the FogState
}

// THE FOG THE SHADER READS, derived from the LAST description the host pushed
// AND the sky — re-derived whenever either changes, which is the point of it
// being a function (the colour mode used to be decided once, at setFog time,
// and outlived the sky it was made of).
//
// Under the planet's atmosphere the colour is that sky's own radiance for
// every layer — there is no switch, the sky decides — and the air's aerial
// perspective is the piece's whether or not the World fog is on; the
// component's block is then an identity (density 0). Under any other sky the
// World fog is the only medium and upstream's block does it in the authored
// colour.
void OgreScene::pushFogState() {
    const bool fogOn = mAtmoFogOn && mFogDescKnown;
    const bool airRead = mAtmoSkyOn && mAtmosphere && mAtmosphere->aerialScale() > 0.0f;
    pushHeightFog();
    if (!mAtmosphere || (!fogOn && !airRead && !mHeightFogOn)) {
        FogHlmsListener::unregisterFog(mSceneMgr);
        return;
    }
    if (!fogOn && !airRead) {
        // THE HEIGHT FOG ALONE (SKY-DEFAULTS-1): the component is registered for
        // its own block, and the World fog's state is the identity — no colour,
        // no height layer, no aerial table claimed, upstream's block at density 0.
        FogHlmsListener::registerScene(mSceneMgr, FogState());
        mAtmosphere->setFogBlock(0.0f, 0.0f, 0.0f);
        return;
    }
    const FogDesc &desc = mLastFogDesc;
    FogState s;
    if (fogOn) {
        s.r = desc.colour.r; s.g = desc.colour.g; s.b = desc.colour.b;
        s.heightDensity = std::max(desc.heightDensity, 0.0f);
        s.heightFalloff = desc.heightFalloff;
        s.heightLevel   = desc.heightLevel;
    }
    s.atmosphere = mAtmoSkyOn;
    const float worldDensity = fogOn ? std::max(desc.density, 0.0f) : 0.0f;
    // Under the atmosphere the World fog's density rides our pass buffer (the
    // piece fogs per pixel towards the sky), and upstream's block is left an
    // identity; under any other sky the block carries it.
    s.distanceDensity = mAtmoSkyOn ? worldDensity : 0.0f;
    FogHlmsListener::registerScene(mSceneMgr, s);   // read by preparePassHash / preparePassBuffer
    // The breakthrough is the World fog's authored bend of the curve (the
    // piece reads the pair from the same buffer under the atmosphere).
    mAtmosphere->setFogBlock(mAtmoSkyOn ? 0.0f : worldDensity,
                             fogOn ? std::max(desc.breakMinBrightness, 0.0f) : 0.0f,
                             fogOn ? std::max(desc.breakFalloff, 0.0f) : 0.0f);
}

// ---------------------------------------------------------------------------
// THE HEIGHT FOG (HeightFogDesc; SKY-DEFAULTS-1)
// ---------------------------------------------------------------------------
// The atmosphere component's third customer. It needs none of the atmosphere's
// tables: its colour is a constant per environment (the sky's SH at +Y) and its
// medium is a closed-form integral, so it rides the component's const buffer
// (the PBS passes, through the JahFog piece) and the component's second quad
// (the sky's pixels). Under ANY sky: a colour sky's in-scatter is its colour.
void OgreScene::setHeightFog(const HeightFogDesc &desc) {
    const bool on = desc.enabled && desc.density > 0.0f;
    if (on == mHeightFogOn && (!on || desc == mHeightFog)) return;
    mHeightFog = desc;
    mHeightFogOn = on;
    // NO probe-grid stale: the height fog never reaches a probe face (its pass
    // property is withheld while a cubemap probe renders, JahAtmosphere::
    // preparePassHash), so the probes' photograph of the world is the same with
    // it on or off.
    if (on) {
        ensureAtmosphere();
        if (!mAtmosphere) return;   // media missing: mError says why
        syncAtmosphere();
        return;
    }
    if (!mAtmosphere) return;
    if (mAtmoSkyOn || mAtmoFogOn) { syncAtmosphere(); return; }
    // Nobody else wants the component: gone, exactly as the World fog leaves it.
    destroyAtmosphere();
    FogHlmsListener::unregisterFog(mSceneMgr);
}

// THE COLOUR IN FORCE: the mean radiance an upward-facing surface receives from
// the environment in force (the SH, Engine.h setAmbientSh's basis
// 1, y, z, x, xy, yz, 3z^2 - 1, zx, x^2 - y^2 evaluated at n = +Y), UNSCALED by
// the Sky Light: the fog is lit by the sky, not by the node that lights the
// scene with it. No environment yet: black, and the status says so.
void OgreScene::pushHeightFog() {
    if (!mAtmosphere) return;
    float rgb[3] = { 0.0f, 0.0f, 0.0f };
    if (mSkyShInForceValid) {
        for (int c = 0; c < 3; ++c) {
            const float *k = mSkyShInForce;
            rgb[c] = std::max(0.0f, k[0 * 3 + c] + k[1 * 3 + c] - k[6 * 3 + c] - k[8 * 3 + c]);
        }
    }
    JAH_TRY {
        mAtmosphere->setHeightFog(mHeightFogOn, mHeightFog.density, mHeightFog.heightFalloff,
                                  mHeightFog.baseHeight, std::max(0.0f, mHeightFog.startDistance),
                                  rgb, kSunDiscBit);
    } JAH_CATCH(mError, );
}

HeightFogStatus OgreScene::heightFogStatus() const {
    HeightFogStatus st;
    if (!mAtmosphere || !mHeightFogOn) return st;
    st.on = mSceneMgr->getAtmosphereRaw() == mAtmosphere;
    const Ogre::Vector3 c = mAtmosphere->heightFogColour();
    st.colour[0] = c.x; st.colour[1] = c.y; st.colour[2] = c.z;
    st.colourFromSky = mSkyShInForceValid;
    return st;
}

}}}  // namespace jahshaka::engine::detail
