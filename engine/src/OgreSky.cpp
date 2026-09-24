// The sky (Ogre's own SceneManager::setSky) and the GGX-prefiltered IBL
// reflection cubemap derived from it.
//
// There is no Jahshaka sky GEOMETRY any more. Until the Ogre adoption wave this
// file built a UV sphere for equirect skies and six inward quads for cube skies,
// scaled them to the far plane and re-centred them on the camera every frame.
// Ogre draws the sky as a Rectangle2D at the far plane whose fragment shader
// turns the interpolated camera direction into a lat-long or cube lookup: no
// mesh, no datablock, no per-frame node work, and correct in every view that
// shares the scene (Ogre feeds the rectangle each camera's corner rays).
#include <vector>
#include "EnginePrivate.h"

#include <cstdlib>

#include <OgreBitwise.h>
#include <OgreMaterial.h>
#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreTextureUnitState.h>
#include <OgreMaterialManager.h>
#include <OgreControllerManager.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreShaderParams.h>
#include <OgreStagingTexture.h>
#include <OgreAsyncTextureTicket.h>
#include <cstring>

namespace jahshaka { namespace engine { namespace detail {

namespace {

// DESTROY A TEXTURE AND GIVE ITS NAME BACK (lane shadercache-2). Every cube
// this file creates wears a RECYCLED name — see recycledName() in
// EnginePrivate.h for why a fresh one per capture costs a permanent Hlms
// pass-cache entry and a shader compile — and a recycled name is only recycled
// if it is released when the texture dies. Safe for any texture: a name this
// pool never handed out is ignored.
void destroyRecycled(Ogre::TextureGpuManager *tm, Ogre::TextureGpu *tex) {
    if (!tm || !tex) return;
    const std::string name = tex->getNameStr();
    tm->destroyTexture(tex);
    releaseRecycledName(name);
}
// Ogre's cubemap lookups are LEFT-handed (see buildCubeFromWorldFaces' comment).
// Destination slice d takes source WORLD face kSrcFace[d], mirrored as flagged.
const int  kSrcFace[6] = { 0, 1, 2, 3, 5, 4 };   // +Z and -Z swap
const bool kFlipH[6]   = { true, true, false, false, true, true };
const bool kFlipV[6]   = { false, false, true, true, false, false };

// A CUBE WE DREW OURSELVES MUST BE HANDED OVER IN THE LAYOUT A SAMPLER EXPECTS
// (ENVPROBE-LAYOUT-1, 2026-09-18). Everything HlmsPbs samples has to be in
// `ResourceLayout::Texture` — the Vulkan render system writes
// `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` into the descriptor whatever the
// image's real layout is (`VulkanRenderSystem::_setTexture`), and its own
// debug-high check throws "Did you forget to expose it to compositor?" for a
// texture that is not. A compositor NODE gets this for free: the next pass that
// names the texture resolves the transition. THIS engine's reflection cubemap
// is written by a workspace of its own (`applyPendingIbl`) and then handed
// straight to the datablocks, so no later pass names it and nothing moved it
// out of the `Uav` layout the convolution left it in:
// `VUID-vkCmdDraw-None-09600` on ten subresources of that one cube, twice
// each, on an ordinary editor run (a new project with one cube — the repro is
// in OGRE_UPSTREAM_ISSUES.md). The driver READ it correctly, which is why no
// picture was ever wrong and why this stood for days; sampling an image in
// GENERAL is legal for the hardware and illegal by the descriptor rule, so it
// is the class that works until a driver stops tolerating it.
//
// Called INSIDE the frame (applyPendingIbl runs at the top of
// renderOneFrame), which is where a command buffer exists. The array is a
// LOCAL and not the solver's shared scratch (`getNewResourceTransitionsArrayTmp`
// says not to hold that in two places), and `executeResourceTransition` closes
// every open encoder itself.
void handOverForSampling(Ogre::Root *root, Ogre::TextureGpu *tex) {
    if (!root || !tex) return;
    Ogre::RenderSystem *rs = root->getRenderSystem();
    if (!rs) return;
    // AND THE COPY ENCODER IS CLOSED FIRST. The fallback route below writes
    // the cube with `copyTo` + `_autogenerateMipmaps`, which leaves it
    // `CopyEncoderManaged` — and `resolveTransition` asserts exactly that
    // ("Call RenderSystem::endCopyEncoder first!", OgreResourceTransition.cpp
    // ~206) because the solver cannot reason about a texture the encoder still
    // owns. Closing it also hands the solver the layout the encoder left
    // (`assumeTransition`), so the resolve below starts from the truth. A no-op
    // with no copy encoder open, which is the convolution's own route.
    rs->endCopyEncoder();
    Ogre::BarrierSolver &solver = rs->getBarrierSolver();
    Ogre::ResourceTransitionArray trans;
    // The PIXEL stage alone: this cube is `texEnvProbeMap` in HlmsPbs, and the
    // mask only narrows the barrier's destination stages.
    solver.resolveTransition(trans, tex, Ogre::ResourceLayout::Texture,
                             Ogre::ResourceAccess::Read, 1u << Ogre::PixelShader);
    rs->executeResourceTransition(trans);
}

const char *kIblWorkspace = "JahshakaIblSpecularWorkspace";
const char *kSkyCaptureWorkspace = "JahshakaSkyCaptureWorkspace";

// THE CAPTURED SKY'S FACE SIZE. 128 is what the host's CPU resample produced
// for every equirect/baked sky before SKY-GPU, so the reflections this replaces
// are the same resolution; the SH is integrated from the 32^2 mip of it, which
// is what the cubemap path always integrated. Raising it costs one GGX
// convolution, not one CPU pass — but it changes every reflection in every
// scene, so it is a deliberate number, not a knob.
const Ogre::uint32 kSkyCaptureSize = 128u;
/// ...and the mip whose faces the ambient integral reads (128 >> 2 = 32).
const Ogre::uint8  kSkyShMip = 2u;

// THE SIX CAPTURE FACES, in the camera basis Ogre's own
// CompositorPass::CubemapRotations gives a `camera_cubemap_reorient` pass
// (OgreCompositorPass.cpp:59), applied to a camera at the origin with the
// identity orientation: forward = q*(0,0,-1), right = q*(1,0,0), up = q*(0,1,0).
//
// It is NOT the world-face basis of buildCubeFromWorldFaces above — slice 4 of
// an Ogre cube ("+Z") is the world -Z direction, and four of the six faces are
// mirrored against their world-axis twin. That is the same left-handedness the
// kSrcFace/kFlipH/kFlipV table encodes, arrived at from the other end: here the
// GPU wrote the face, so the direction of a texel is the direction the CAMERA
// had, and these three vectors are that camera's axes. Deriving it twice and
// getting the same answer is the check that matters (dst 4 <- world face 5 with
// a horizontal flip, exactly as the table says).
const float kFaceFwd[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,-1}, {0,0,1} };
const float kFaceRight[6][3] = { {0,0,1}, {0,0,-1}, {1,0,0}, {1,0,0}, {1,0,0}, {-1,0,0} };
const float kFaceUp[6][3] = { {0,1,0}, {0,1,0}, {0,0,1}, {0,0,-1}, {0,1,0}, {0,1,0} };

/// Cosine-convolved irradiance in 9 SH bands, in the basis and the units
/// Scene::setAmbientSh documents (its header has the whole model). Moved here
/// from the host with the integral itself: the sky is a GPU cube now, and the
/// host has no way to read one.
struct ShAccum {
    double a[9][3] = {};
    void add(float x, float y, float z, double r, double g, double b, double w) {
        const double bi[9] = { 1.0, y, z, x, double(x) * y, double(y) * z,
                               3.0 * double(z) * z - 1.0, double(z) * x,
                               double(x) * x - double(y) * y };
        for (int i = 0; i < 9; ++i) {
            const double f = bi[i] * w;
            a[i][0] += r * f; a[i][1] += g * f; a[i][2] += b * f;
        }
    }
    void finish(float out[27]) const {
        static const double k[9] = {
            0.0795774715,
            0.1591549431, 0.1591549431, 0.1591549431,
            0.2984155183, 0.2984155183,
            0.0248679599,
            0.2984155183,
            0.0746038796
        };
        for (int i = 0; i < 9; ++i)
            for (int c = 0; c < 3; ++c) out[i * 3 + c] = float(a[i][c] * k[i]);
    }
};
}  // namespace

// THE ONE SKY ENTRY POINT (ENGINEERING_DEBT_SPEC.md item 4). It owns the three
// things the host used to: the dispatch on mode, the ordering (sky first, then
// reflections — destroySky takes the reflection cubemap with it, so the reverse
// order would throw away reflections that were just built), and the
// already-applied comparison, per HALF: a description that changes only its
// reflection faces must not tear the sky down and back up, because that is a
// texture upload plus a six-face cube build for nothing.
bool OgreScene::setSky(const SkyDesc &desc) {
    // NoSky is a FULL clear: a description with no sky and no opinion on
    // reflections must not leave an IBL-only reflection bound (the contract
    // EnginePrivate.h states and the mirror's setSource relies on; code review
    // 2026-09-10).
    const bool noSkyClearsIbl = desc.mode == SkyMode::NoSky && mSkyDesc.reflections && !desc.reflections;
    const bool skyChanged  = !mSkyDesc.sameSky(desc) || noSkyClearsIbl;
    const bool reflChanged = !mSkyDesc.sameReflections(desc);
    // THE SUN DISC is the description's third independent half: it changes
    // every time the sun light is rotated, and re-uploading the sky or
    // re-convolving the IBL cubemap for that would be absurd. It also does NOT
    // stale the probe grid — the disc is excluded from probe captures by
    // default, and when it is not, moving the sun already stales them through
    // the light itself.
    const bool sunChanged  = !(mSkyDesc.sun == desc.sun);
    if (sunChanged) {
        // ...WITH ONE EXCEPTION, and it is the whole of ENGINE-6 item 5: while
        // the disc IS in the probe captures it is part of what they
        // photograph, so a change to it has to invalidate them exactly as a sky
        // change does. Nothing did, and since the probe captures became CACHED
        // (ENGINE_CACHE_POLICY_SPEC) a cached face is what a mirror keeps
        // showing — which is why `world.sunDisc({inProbes:true})` appeared to
        // do nothing at all in a reflection while the disc drew perfectly in
        // every ordinary camera. It was never a cull or a mask: it was a stale
        // capture. The test is the disc's probe participation BEFORE or AFTER,
        // so turning it off invalidates too (the disc has to leave the faces it
        // is baked into), and the ordinary case — a sun rotating with the disc
        // out of the probes — still stales nothing.
        const bool inCaptures = (mSkyDesc.sun.enabled && mSkyDesc.sun.inProbes) ||
                                (desc.sun.enabled && desc.sun.inProbes);
        applySunDisc(desc.sun);
        mSkyDesc.sun = desc.sun;
        if (inCaptures) {
            staleProbeGrid(GiStaleReason::Sky);
            // ...and the SKY capture with them: while the disc is in the
            // environment it is part of the cube every reflective material
            // samples, so moving the sun moves the reflected sun too.
            if (mSkyDesc.mode != SkyMode::NoSky) requestSkyCapture();
        }
    }
    // THE SUN'S AIR is a fourth independent piece (lane SKY-DENSITY-1), for the
    // same reason as the disc above: `sunHaze` is the only input to
    // atmosphereSunTint and it changes NO sky pixel and no reflection, so
    // dragging it must not tear the sky down, re-render the six capture faces,
    // re-convolve the IBL cube or stale the probe grid. It is not part of
    // AtmosphereSky's equality (Types.h says why); it is applied here.
    if (desc.mode == SkyMode::Atmosphere &&
        desc.atmosphere.sunHaze != mSkyDesc.atmosphere.sunHaze) {
        mSkyDesc.atmosphere.sunHaze = desc.atmosphere.sunHaze;
        mAtmoSunHaze = std::max(1.0f, desc.atmosphere.sunHaze);
        ++mAtmoPresetGeneration;   // the tint's memo is keyed on this
    }
    // THE CLOUD LAYER (CLOUDS-2D-1) is the fourth independent half. Three kinds
    // of change, three costs: the FIELD (coverage, density, the weather map)
    // re-bakes the optical-depth field; the LOOK (the field, the altitude, the
    // sun that lights it) re-captures the environment, immediately, and stales
    // the probe grid, which photographs the sky; the WIND and the shadow
    // strength move no sky pixel at a given time and cost a uniform write. The
    // scroll's own re-captures are the cadence's (tickCloudClock), not this.
    if (!(mSkyDesc.clouds == desc.clouds)) {
        const bool fieldChanged = !mSkyDesc.clouds.sameField(desc.clouds);
        const bool lookChanged = !mSkyDesc.clouds.sameLook(desc.clouds);
        mSkyDesc.clouds = desc.clouds;
        applyCloudLayer(fieldChanged);
        if (lookChanged && mSkyDesc.mode != SkyMode::NoSky && !skyChanged) {
            staleProbeGrid(GiStaleReason::Sky);
            requestSkyCapture();
            ++mCloudStatus.changeCaptures;
            mCloudFramesSinceCapture = 0u;
        }
    }
    if (!skyChanged && !reflChanged) return true;   // idempotent: nothing else to do
    // THE PROBE CACHE'S SKY INPUT (ENGINE_CACHE_POLICY_SPEC P7): the probe
    // faces capture the sky (RQ 0 is inside their range) and the reflection
    // cubemap lights what they capture. Nothing staled a probe on a sky change
    // before; the endless sweep was the only thing that ever showed one.
    staleProbeGrid(GiStaleReason::Sky);
    bool ok = true;
    if (skyChanged) {
        if (applySkyMode(desc)) {
            mSkyDesc.mode = desc.mode;
            mSkyDesc.equirect = desc.equirect;
            mSkyDesc.atmosphere = desc.atmosphere;
            for (int i = 0; i < 6; ++i) mSkyDesc.faces[i] = desc.faces[i];
            // THE ENVIRONMENT COMES FROM THE SKY ITSELF (SKY-GPU). Every sky
            // there is gets CAPTURED on the GPU — six render_scene passes over
            // the sky's render queue — and that cube is what the ambient
            // integral reads. For every sky but a cubemap it is ALSO the IBL
            // convolution's input; a cubemap sky keeps feeding the convolution
            // its own full-resolution faces, because re-rendering those into a
            // 128^2 capture would throw reflection detail away for nothing.
            // The layer is drawn over any sky there is (the HOST keeps it off
            // image skies, which carry their own clouds) — a mode change can
            // turn it on or off.
            // A new sky is also a new light on the sheet: its clear-sky term is
            // re-taken before the capture that photographs it.
            mCloudClearPending = true;
            applyCloudLayer(false);
            if (desc.mode != SkyMode::NoSky)
                requestSkyCapture();
            else {
                // No sky, no sky light: the ambient the host derives from this
                // must go to zero in the same push that removed the sky.
                mSkyCapturePending = false;
                forgetSkySh();
                destroySkyShTicket();
            }
            // Both of these paths REPLACE the reflection cubemap themselves —
            // destroySky() unbinds and frees it, and a cubemap sky rebuilds it
            // from its own faces — so whatever reflection description was in
            // force no longer describes anything. Forgetting it here is what
            // lets the host push the very same reflection faces afterwards and
            // have them actually applied.
            if (desc.mode != SkyMode::Equirectangular) {
                mSkyDesc.reflections = false;
                for (int i = 0; i < 6; ++i) mSkyDesc.reflectionFaces[i] = 0;
            }
        } else {
            ok = false;
        }
    }
    // `reflections == false` is "no opinion": leave whatever is bound alone.
    if (desc.reflections && !mSkyDesc.sameReflections(desc)) {
        if (applySkyReflectionFaces(desc.reflectionFaces)) {
            mSkyDesc.reflections = true;
            for (int i = 0; i < 6; ++i) mSkyDesc.reflectionFaces[i] = desc.reflectionFaces[i];
        } else {
            ok = false;
        }
    }
    return ok;
}

bool OgreScene::applySkyMode(const SkyDesc &desc) {
    if (desc.mode == SkyMode::Cubemap) return applySkyCubemap(desc.faces);
    if (desc.mode == SkyMode::Atmosphere) return applySkyAtmosphere(desc.atmosphere);
    JAH_TRY {
        if (desc.mode == SkyMode::NoSky) { destroySky(); return true; }
        auto it = mTextures.find(desc.equirect);
        if (it == mTextures.end()) { mError = "setSky: unknown sky texture"; return false; }
        Ogre::TextureGpu *src = it->second.texture;
        // WAIT FOR THIS ONE TEXTURE (THREADING_ADOPTION_SPEC.md P2). Everything
        // below reads the texture ITSELF rather than binding it: its internal
        // type, and — inside Ogre's setSky — its POOL SLICE, written into the
        // sky material as a one-shot `sliceIdx` uniform. A texture that is still
        // streaming answers Type2D and slice 0, so the sky would render whatever
        // else happens to be in slice 0 of that pool, for ever, silently. See
        // waitForTextureResident in EnginePrivate.h.
        waitForTextureResident(src);
        // SkyEquirectangular is hard-gated on getInternalTextureType() ==
        // Type2DArray (OgreSceneManager.cpp:1125). File-loaded textures are
        // automatic-batching pool slices and already qualify; pixel-uploaded
        // ManualTextures do not, so they get a one-slice array copy.
        Ogre::TextureGpu *use = src;
        Ogre::TextureGpu *owned = nullptr;
        if (src->getInternalTextureType() != Ogre::TextureTypes::Type2DArray) {
            owned = makeSkyArrayTexture(src);
            if (!owned) return false;   // mError set
            use = owned;
        }
        Ogre::TextureGpu *previous = mSkyOwnedTex;
        mSkyOwnedTex = owned;
        // Leaving the analytic sky: its quad is hidden here rather than in
        // destroySky, because an image sky does not tear anything down — and
        // two sky quads at render queue 0 would both draw.
        if (mAtmoSkyOn) { mAtmoSkyOn = false; syncAtmosphere(); }
        mSceneMgr->setSky(true, Ogre::SceneManager::SkyEquirectangular, use);
        tuneSkyRenderable();
        // Only now is the old texture unreferenced by the sky material. If a
        // reflection convolution was still PENDING from that texture (a cubemap
        // sky is its own IBL source), it must not run against a destroyed one
        // on the next frame (code review 2026-09-10).
        if (previous && previous != owned) {
            if (mIblSourceTex == previous && !mIblSourceOwned) destroyPendingReflection();
            destroyRecycled(mRoot->getRenderSystem()->getTextureGpuManager(), previous);
        }
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::applySkyCubemap(const TextureId faces[6]) {
    JAH_TRY {
        Ogre::TextureGpu *tex[6];
        for (int i = 0; i < 6; ++i) {
            auto it = mTextures.find(faces[i]);
            if (it == mTextures.end()) { mError = "setSky: unknown cubemap face texture"; return false; }
            tex[i] = it->second.texture;
        }
        // The faces are READ here, not bound: their resolution decides the cube's,
        // and buildCubeFromWorldFaces copies their PIXELS. Both need them resident
        // (THREADING_ADOPTION_SPEC.md P2 — loadTexture only schedules).
        for (int i = 0; i < 6; ++i) waitForTextureResident(tex[i]);
        for (int i = 0; i < 6; ++i)
            if (tex[i]->getWidth() != tex[0]->getWidth() || tex[i]->getHeight() != tex[0]->getHeight()) {
                mError = "setSky: the six cubemap faces must all be the same size";
                return false;
            }
        // ONE cube serves as both the sky and the IBL convolution input — the two
        // hold identical pixels, and for a 2K cubemap sky that is 100 MB and a
        // download/flip/upload round trip saved. Hence the mip chain and
        // AllowAutomipmaps even though the sky itself only ever reads mip 0: the
        // ibl_specular pass regenerates the input's mips before integrating.
        const bool square = tex[0]->getWidth() == tex[0]->getHeight();
        Ogre::TextureGpu *cube = buildCubeFromWorldFaces(
            tex, "skycube",
            square ? (Ogre::TextureFlags::RenderToTexture | Ogre::TextureFlags::AllowAutomipmaps) : 0u,
            square);
        if (!cube) return false;   // mError set
        Ogre::TextureGpu *previous = mSkyOwnedTex;
        // destroyReflection() below must not free the cube we are about to hand
        // the sky, so clear the alias before it runs.
        mSkyOwnedTex = nullptr;
        // Environment reflections: the same cube becomes the GGX-prefiltered
        // cubemap on every PBR datablock's reflection slot, so metals and glass
        // mirror the sky the way the legacy matcap/refraction shaders faked it.
        if (square) buildReflectionCubemapFrom(cube, false);
        else        destroyReflection();
        mSkyOwnedTex = cube;
        if (mAtmoSkyOn) { mAtmoSkyOn = false; syncAtmosphere(); }
        mSceneMgr->setSky(true, Ogre::SceneManager::SkyCubemap, cube);
        tuneSkyRenderable();
        if (previous)
            destroyRecycled(mRoot->getRenderSystem()->getTextureGpuManager(), previous);
        return true;
    } JAH_CATCH(mError, false);
}

// ---------------------------------------------------------------------------
// THE ANALYTIC SKY — Ogre's AtmosphereNpr (SKY-GPU, owner pick 5)
// ---------------------------------------------------------------------------
// The "realistic" sky used to be a Preetham evaluation the HOST ran on the CPU,
// up to 1024x512 pixels of pow/exp/acos per parameter change, on the UI thread,
// into an equirect image that was then uploaded, resampled into six cube faces
// and integrated for its ambient. The engine has its own analytic sky —
// Components/Atmosphere, already built and already in this process for the FOG
// — whose whole model is a fragment shader over the camera ray. This is the
// adoption (ALL-IN ON OGRE): the sky is evaluated where a sky belongs.
//
// THREE THINGS THE COMPONENT DOES THAT WE DO NOT WANT, and how each is refused:
//
//   1. IT OVERWRITES THE LINKED LIGHT. syncToLight() sets the light's type,
//      direction, diffuse AND specular colour and power scale from its own
//      model, and pushes an ambient hemisphere pair into the SceneManager
//      (OgreAtmosphereNpr.cpp:199-213). All of that is OURS: the sun is the
//      user's directional light and the ambient is the Sky Light's SH. The
//      refusal costs nothing and needs no patch — syncToLight() returns at its
//      first line when no light is linked, and we never call setLight(). The
//      link runs the OTHER way instead: the host pushes the sun light's
//      direction into `sunDir` and this pushes it into the component.
//
//   2. ITS OWN SUN DISC. `sunPower` scales a pow(LdotV, ...) term in the sky
//      shader; zero removes it. The disc is SunDisc's — one mechanism, over
//      every sky type, at the sun light's angular size, with its own visibility
//      channel so probe captures can exclude it. Two discs would be two suns.
//
//   3. ITS FOG, IN EVERY PBS SHADER. preparePassHash sets HlmsBaseProp::Fog for
//      any scene the component is registered on, and registration is not
//      optional: _update() — which writes the quad's per-camera corner rays —
//      only runs for the SceneManager's registered atmosphere. So a scene with
//      the analytic sky compiles the fog block whether or not the World fog is
//      on. `fogDensity = 0` makes that block an exact identity (fogWeight =
//      exp2(0) = 1, and lerp(a, b, 1) is b), which is what setFog leaves behind
//      when the fog is off. A scene with no analytic sky and no fog registers
//      no atmosphere at all and its shaders are untouched — that is what keeps
//      every other scene's pixels, and the selftest, where they were.
bool OgreScene::applySkyAtmosphere(const AtmosphereSky &sky) {
    ensureAtmosphere();
    if (!mAtmosphere) return false;   // media missing: mError says so
    JAH_TRY {
        // Ogre's own sky (an image) and ours cannot both be bound: one scene,
        // one sky. Dropping it also frees the texture copy it owned.
        if (mSceneMgr->getSky())
            mSceneMgr->setSky(false, mSceneMgr->getSkyMethod(), static_cast<Ogre::TextureGpu *>(nullptr));
        if (mSkyOwnedTex) {
            // A convolution still pending from that texture (a cubemap sky is its
            // own IBL source) must not read it after it dies — applySkyMode's guard.
            if (mIblSourceTex == mSkyOwnedTex && !mIblSourceOwned) destroyPendingReflection();
            destroyRecycled(mRoot->getRenderSystem()->getTextureGpuManager(), mSkyOwnedTex);
            mSkyOwnedTex = nullptr;
        }

        Ogre::AtmosphereNpr::Preset preset = mAtmosphere->getPreset();
        preset.densityCoeff     = std::max(0.0f, sky.density);
        preset.densityDiffusion = std::max(0.0f, sky.diffusion);
        preset.horizonLimit     = sky.horizon;
        preset.skyColour        = Ogre::Vector3(sky.skyColour.r, sky.skyColour.g, sky.skyColour.b);
        preset.skyPower         = std::max(0.0f, sky.skyPower);
        preset.sunPower         = 0.0f;    // (2) above: the disc is SunDisc's
        // THE FOG HALF belongs to setFog — but only when the fog is ON. When
        // this path is what CREATED the component (a scene that picks the
        // analytic sky and has never touched the fog), the preset it copies is
        // UPSTREAM's constructor default, and that is `fogDensity( 0.0001f )`
        // (OgreAtmosphereNpr.h): 0.7 % of a surface's colour lost at 100 m and
        // 13 % at the 2 km horizon plane, for a fog nobody asked for and no
        // panel row admits to. setFog's off-branch zeroes it, but only for a
        // scene that had the fog on first. Zero it here, from the flag that
        // says whether the fog has a customer at all.
        if (!mAtmoFogOn) preset.fogDensity = 0.0f;
        mAtmosphere->setPreset(preset);
        // THE SUN'S OWN AIR IS NOT A PRESET FIELD (lane SKY-DENSITY-1): the
        // component draws the sky, the transmittance below the atmosphere is
        // ours, and nothing about this number reaches the sky pass. Held at or
        // above a purely molecular atmosphere — under 1 the aerosol term would
        // turn negative and AMPLIFY the beam.
        mAtmoSunHaze = std::max(1.0f, sky.sunHaze);
        ++mAtmoPresetGeneration;   // atmosphereSunTint's memo is keyed on this

        // THE SUN, PUSHED IN. setSunDir takes the direction the light TRAVELS
        // (it negates internally: mSunDir = -sunDir) plus a normalised time of
        // day, which the model uses for the sun's height terms; sin(elevation)
        // of our own unit vector is that number, and asin/PI puts it in the
        // [0;1] the component wants. With no sun light in the scene the sky is
        // evaluated at the model's lowest sun: its night, the same answer the
        // CPU bake gave for a sunless scene.
        const Ogre::Vector3 toSun = sky.hasSun
            ? Ogre::Vector3(sky.sunDir[0], sky.sunDir[1], sky.sunDir[2]).normalisedCopy()
            : Ogre::Vector3::UNIT_Y;
        const float elevation = std::max(-1.0f, std::min(1.0f, float(toSun.y)));
        const float timeOfDay = sky.hasSun
            ? std::max(0.0f, std::min(1.0f - 1e-6f, std::asin(elevation) / float(M_PI)))
            : 0.0f;
        mAtmosphere->setSunDir(-toSun, timeOfDay);
        mAtmoSunDir = -toSun;          // what the component is holding, for the tint query
        mAtmoTimeOfDay = timeOfDay;

        mAtmoSkyOn = true;
        syncAtmosphere();
        return true;
    } JAH_CATCH(mError, false);
}

// THE ATMOSPHERE'S TINT ON THE DIRECT SUNLIGHT (SUN_FOLLOWS_ATMOSPHERE, lane
// ENGINE-7 item 6; the model below is lane SKY-DENSITY-1's; Engine.h states the
// contract).
//
// WHAT IT IS. The fraction of the sun's beam that survives the trip down, per
// channel, relative to the trip it makes at the zenith. That is Beer-Lambert
// along ONE ray:
//
//     T(elevation) = exp( -tau * m(elevation) )
//     tint         = T(elevation) / T(90 degrees)
//
// with `m` the relative AIRMASS and `tau` the atmosphere's optical depth per
// channel. Divided by its own value at the zenith so the answer is exactly
// (1,1,1) at noon — the user's picked colour IS the noon colour — and falls,
// blue first, as the sun goes down.
//
// WHY IT IS NOT THE SKY'S DIAL ANY MORE (the defect this lane closes). Until
// 2026-09-15 this quantity was read out of AtmosphereNpr's own preset:
//
//     lightDensity = densityCoeff / max(sunHeight, 0.0035)^0.75
//     absorption   = 2 * exp2(-lightDensity * skyColour)
//
// — the NPR model's internal absorption term, which meant `densityCoeff` set
// BOTH the sky dome's look and the colour of the sunlight. The two are
// different physical quantities: the sky's radiance is an integral of
// scattering over a whole view ray (and AtmosphereNpr is explicitly NOT a
// physical model of it — its density is an artistic dial, fitted by SKY-TUNE-1
// to a Preetham reference at turbidity 2.5), while the sun's colour is the
// extinction along the single ray to the sun, which needs no art at all. One
// dial for two jobs meant every sky tune moved the sunlight and every sunlight
// tune moved the sky: SKY-TUNE-1 measured the residual at 0.046 stops when the
// dial was where the SUN wanted it (0.47) and 0.189 stops where the SKY wanted
// it (0.25), and had to ship a compromise inside the joint optimum.
//
// THE MODEL, AND WHERE ITS NUMBERS COME FROM. The optical depths are the
// standard clear-atmosphere terms of Preetham et al. 1999 (appendix A.2, the
// direct solar attenuation) — the SAME model, at the same turbidity, the sky's
// own defaults were fitted to, so the sky and the sunlight now describe one
// atmosphere through two dials instead of disagreeing through one:
//
//     tau_rayleigh(l) = 0.008735 * l^-4.08                   (l in micrometres)
//     tau_aerosol(l)  = beta * l^-1.3,  beta = 0.04608*T - 0.04586   (Angstrom)
//     tau_ozone(l)    = k_o(l) * 0.35 cm                     (the Chappuis band)
//
// evaluated at 600 / 550 / 450 nm for linear sRGB R / G / B, and `T` is the
// atmosphere's Linke TURBIDITY — the one dial, `AtmosphereSky::sunHaze`
// (1 = purely molecular, 2.5 = the clear day the sky was fitted to, 4-6 hazy).
// The mixed-gas and water-vapour terms of the same model are 760 nm and beyond:
// zero across the visible, so they are not carried.
//
// Airmass is Kasten-Young (1989), which is the one part a low sun cannot do
// without: 1/sin(h) is 28% wrong by 5 degrees and diverges at the horizon,
// while this form is within 0.1% down to zero:
//
//     m(h) = 1 / ( sin(h) + 0.50572 * (h_deg + 6.07995)^-1.6364 )
//
// HOW FAITHFUL IT IS, MEASURED (spikes/skyd/, this lane). Against the same
// model integrated SPECTRALLY at 5 nm from 380 to 750 nm through the CIE 1931
// observer and into linear sRGB — i.e. against what three channels can only
// approximate — the three-wavelength form above agrees to 0.04 stops at a
// 30-degree sun, 0.08 at 20, 0.19 at 10 and 0.39 at 5 (R and G; by then B is
// under 0.02 in both and the sRGB primaries no longer contain the beam). The
// old preset-derived form was 0.7 to 3.3 stops BRIGHT over the same range —
// it lost 0.7 stops by a 5-degree sun where the air really takes 3.7.
//
// WHAT MOVED, AT THE SHIPPED DEFAULTS (haze 2.5, and it is only the SUN that
// moved — no sky pixel reads this function):
//
//     elevation   old (density 0.25)      new (haze 2.5)      reference
//        30 deg   0.961 0.935 0.889      0.781 0.756 0.656   0.804 0.753 0.642
//        10 deg   0.854 0.765 0.624      0.320 0.276 0.143   0.365 0.268 0.126
//         5 deg   0.739 0.596 0.404      0.099 0.073 0.019   0.130 0.068 0.013
//         2 deg   0.517 0.325 0.139      0.010 0.006 0.000   0.018 0.005 0.000
//
// WHY NOT THE COMPONENT'S OWN LIGHT LINK. `setLight` takes the light over
// completely — type, direction, diffuse, specular and power — so it would
// delete the user's colour and intensity rather than tint them, and its colour
// is normalised to max 1, i.e. it reddens without dimming (and makes the NOON
// sun blue, because the quantity it normalises is the sky's radiance looking at
// the sun, not the sunlight). The link stays unarmed, as SKY-GPU left it.
//
// WHY NOT getAtmosphereAt. That is the sky's in-scattered radiance in a
// direction — it gets BRIGHTER as the sun sets (measured: 0.09/0.24/0.55 at the
// zenith against 6.92/3.38/0.69 at 5 degrees, which is the sunset glow) — so it
// is the wrong quantity for "what reached the ground".
namespace {
// Optical depth per linear-sRGB channel at 600 / 550 / 450 nm (see above).
constexpr float kTauRayleigh[3]   = { 0.07021f, 0.10013f, 0.22707f };
constexpr float kTauOzone[3]      = { 0.04375f, 0.02975f, 0.00105f };
// The Angstrom aerosol term at unit beta: lambda^-1.3 with lambda in microns.
constexpr float kTauAerosolPerBeta[3] = { 1.94269f, 2.17535f, 2.82373f };

/// Kasten-Young (1989) relative airmass. `elevDeg` is the sun's geometric
/// elevation; below the horizon the formula's own guard (the +6.08 offset)
/// keeps it finite, and the Earth's occlusion below takes the answer to zero
/// long before it matters.
inline float relativeAirmass(float elevDeg) {
    const float h = elevDeg * float(M_PI) / 180.0f;
    const float denom = std::sin(h)
        + 0.50572f * std::pow(std::max(elevDeg + 6.07995f, 1e-3f), -1.6364f);
    return denom > 1e-6f ? 1.0f / denom : 1.0f / 1e-6f;
}
}   // namespace

Colour OgreScene::atmosphereSunTint(const Vec3 &toSunIn) const {
    const Colour white(1.0f, 1.0f, 1.0f, 1.0f);
    if (!mAtmosphere || !mAtmoSkyOn) return white;
    Ogre::Vector3 toSun(toSunIn.x, toSunIn.y, toSunIn.z);
    if (toSun.squaredLength() < 1e-12f) return white;
    toSun.normalise();
    if (mAtmoTintGeneration == mAtmoPresetGeneration &&
        (mAtmoTintDir - toSun).squaredLength() < 1e-12f)
        return mAtmoTint;
    Colour tint = white;
    JAH_TRY {
        // THE BEAM'S TRANSMITTANCE, RELATIVE TO THE ZENITH. Only the airmass
        // DIFFERENCE survives the ratio, so the absolute column (which a
        // renderer has no use for — the user's sun colour is the noon colour by
        // contract) cancels and the whole model is three exponentials.
        const float beta = std::max(0.0f, 0.04608f * mAtmoSunHaze - 0.04586f);
        const float elevDeg = float(std::asin(std::max(-1.0, std::min(1.0, double(toSun.y))))
                                    * 180.0 / M_PI);
        const float dm = relativeAirmass(elevDeg) - relativeAirmass(90.0f);
        float rgb[3];
        for (int c = 0; c < 3; ++c) {
            const float tau = kTauRayleigh[c] + kTauOzone[c] + beta * kTauAerosolPerBeta[c];
            rgb[c] = std::max(0.0f, std::min(1.0f, std::exp(-tau * dm)));
        }
        tint = Colour(rgb[0], rgb[1], rgb[2], 1.0f);
        // ...AND THEN THE EARTH GETS IN THE WAY (lane SUN-DISC-1; the rig's
        // horizon-crossing capture, 2026-09-14).
        //
        // NO TRANSMITTANCE MODEL HAS AN ANSWER BELOW THE HORIZON, and each is
        // wrong in its own way: the preset-derived one this lane replaced FROZE
        // there (its inputs clamped, so a sun 30 degrees under the ground went
        // on lighting the scene at a constant fraction of noon, disc drawn and
        // shadows cast upwards — measured, 2026-09-14), and an airmass formula
        // is fitted to a ray that still reaches the ground, which a ray from
        // below does not. Neither is a reason to guess: the term that ends
        // sunlight is geometry, not chemistry, and it is exact.
        //
        // The Earth occludes the
        // sun. The sun's own disc is 0.53 degrees wide (0.265 of radius) and
        // refraction lifts the apparent disc by about 0.57 degrees at the
        // horizon, so direct sunlight starts to be cut at a GEOMETRIC centre
        // elevation of -(0.57 - 0.265) = -0.305 degrees and has ended by
        // -(0.57 + 0.265) = -0.835 — the astronomical definition of sunset.
        // A smoothstep across that band is the whole fix, and it makes the
        // crossing CONTINUOUS: the light, the disc and the shadow all ride the
        // same tint, so they fade together and the night rule now trips on a
        // value that is already zero instead of deciding when night begins.
        {
            constexpr float kSunSetStartDeg = -0.305f;   // lower limb touches the horizon
            constexpr float kSunSetEndDeg   = -0.835f;   // upper limb goes under
            float occl = 1.0f;
            if (elevDeg <= kSunSetEndDeg) {
                occl = 0.0f;
            } else if (elevDeg < kSunSetStartDeg) {
                const float t = (elevDeg - kSunSetEndDeg) / (kSunSetStartDeg - kSunSetEndDeg);
                occl = t * t * (3.0f - 2.0f * t);        // smoothstep, C1 at both ends
            }
            tint = Colour(tint.r * occl, tint.g * occl, tint.b * occl, 1.0f);
        }
    } JAH_CATCH(mError, white);
    mAtmoTintDir = toSun;
    mAtmoTint = tint;
    mAtmoTintGeneration = mAtmoPresetGeneration;
    return tint;
}

// ONE COMPONENT, TWO CUSTOMERS (the analytic sky and the fog). Registration on
// the SceneManager is what makes the quad update and what sets hlms_fog, so it
// is decided HERE from both flags rather than by whichever of setSky/setFog ran
// last — the bug that would otherwise be written twice is "turning the fog off
// takes the sky down with it".
void OgreScene::syncAtmosphere() {
    if (!mAtmosphere) return;
    JAH_TRY {
        if (!mAtmoSkyOn && !mAtmoFogOn) {
            // Neither: hide the quad AND unregister, which is what makes "no
            // fog" bit-exact (no hlms_fog, no fog code in any shader).
            mAtmosphere->setSky(mSceneMgr, false);
            return;
        }
        // setSky(true) shows the quad and registers; setSky(false) hides and
        // unregisters, so "fog only" is the documented two-step: hide, then
        // register again.
        mAtmosphere->setSky(mSceneMgr, mAtmoSkyOn);
        if (!mAtmoSkyOn) mSceneMgr->_setAtmosphere(mAtmosphere);
        tuneAtmosphereRenderable();
        // ...and the FOG's colour mode with it: the aerial mode is only
        // meaningful while the analytic sky is the sky, so it is re-derived
        // here rather than pinned at the moment setFog happened to run
        // (pushFogState's header has the defect that made this a function).
        pushFogState();
    } JAH_CATCH(mError, );
}

// The component parks its quad at render queue 212 with default visibility
// flags. Both are wrong here, for the same reasons Ogre's own sky is moved in
// tuneSkyRenderable: 212 is inside the OVERLAY pass's range [210,255), so the
// sky would be painted over every gizmo, wire and selection outline in the
// scene; and default flags carry kGiGeometryBit, which is what Instant
// Radiosity casts its rays against — a sky that is GI geometry is a bounce
// surface wrapped around the world.
//
// The quad pointer is taken from the SceneManager's Rectangle2D list at the
// moment the component creates it (ensureAtmosphere), because the component
// keeps its per-SceneManager map private and offers no accessor.
void OgreScene::tuneAtmosphereRenderable() {
    if (!mAtmoQuad) return;
    mAtmoQuad->setRenderQueueGroup(0u);
    // Subgroup 1, for the reason tuneSkyRenderable's own note gives: the
    // hidden-area mesh occupies subgroup 0 of this queue inside a VR eye and
    // this quad has to be behind it to be masked by it.
    mAtmoQuad->setRenderQueueSubGroup(1u);
    mAtmoQuad->setVisibilityFlags(kVisibleBit);
    mAtmoQuad->setCastShadows(false);
}

// ---------------------------------------------------------------------------
// THE SKY, CAPTURED ON THE GPU (SKY-GPU)
// ---------------------------------------------------------------------------
void OgreScene::requestSkyCapture() { mSkyCapturePending = true; }

bool OgreScene::skyAmbientSh(float out[27]) const {
    if (!mSkyShInForceValid) return false;
    for (int i = 0; i < 27; ++i) out[i] = mSkyShInForce[i];
    return true;
}

void OgreScene::forgetSkySh() {
    const bool wasLighting = mSkyShInForceValid;
    mSkyShValid = false;
    mSkyShFresh = false;
    mSkyShInForceValid = false;
    // No sky, no sky light: the ambient it was lighting goes to zero with it.
    if (wasLighting) applySkyAmbient(GiStaleReason::Sky);
}

// THE SKY'S AMBIENT IS FORMED HERE (PHOTON-SKY-TRANSIENT-1): the SH in force
// times the Sky Light gain the host pushed through setEnvironmentLight, or zeros
// with no sky (no Sky Light = gain 0 = zeros: gi.sky_light). The host pushes the
// gain only; it used to form this product itself, one sync after the engine had
// integrated the SH, which put the cube of one sky beside the SH of another on
// every change frame. Only while the engine owns the ambient (a host that never
// pushed a gain, or lit its scene with setAmbient since, keeps its own), and not
// during teardown (the write re-notes the GI arm).
void OgreScene::applySkyAmbient(GiStaleReason why) {
    if (!mSkyAmbientOwned || mDestroying) return;
    const float gain[3] = { mEnvLightGain.r, mEnvLightGain.g, mEnvLightGain.b };
    float sh[27];
    for (int i = 0; i < 27; ++i) sh[i] = mSkyShInForceValid ? mSkyShInForce[i] * gain[i % 3] : 0.0f;
    applyAmbientSh(sh, why);
}

// THE ENVIRONMENT LANDS AS ONE SET (PHOTON-SKY-TRANSIENT-1, measured). A sky
// change used to REPLACE the bound reflection cube inside the capture's frame
// with a newborn one whose convolution was queued for the top of the NEXT frame,
// so every draw of the change frame sampled a cube nothing had written: recycled
// VRAM, NaN and 3e4 half-floats in mip 0 (a ground pixel read 111/28/118 or
// 31/255/32 against 25/29/32 a frame later — the one-frame flash on every sky
// or sun edit). And the SH reached the pixel one host push later than the cube.
//
// THE RULE: the pixel keeps the PREVIOUS environment — cube, SH, gain — until
// the capture, the convolution and the SH of the next one have all landed, and
// then the cube and the coefficients swap here, in one step. For a lone change
// all three land inside the capture's own frame (the capture runs before the
// draw, the convolution is run right behind it, the first SH read is
// synchronous), so the change frame already draws the whole new set. A drag's
// SH read is deferred a frame (integrateSkyShFromCube), and its set lands at the
// next frame's top when that read does — the previous set drawn meanwhile.
void OgreScene::landEnvironmentIfComplete() {
    if (mSkyCapturePending || mSkyShTicket || mIblPending) return;   // a part is owed
    if (mReflPendingTex) {
        Ogre::TextureGpu *next = mReflPendingTex;
        mReflPendingTex = nullptr;
        // DESTROY THE OLD FIRST, then bind the new — destroyReflection's order
        // and reason (its Deleted listener is what kills the datablocks' stale
        // descriptor sets). The new cube is a different, live texture, so the
        // recycled-address trap cannot alias it.
        if (Ogre::TextureGpu *old = mReflectionTex) {
            mReflectionTex = nullptr;
            try { destroyRecycled(mRoot->getRenderSystem()->getTextureGpuManager(), old); }
            catch (Ogre::Exception &e) { mError = e.getFullDescription(); }
            catch (std::exception &e)  { mError = std::string("engine: ") + e.what(); }
        }
        mReflectionTex = next;
        // TELL HlmsPbs HOW MANY MIPS THE PROBE HAS. Without this the
        // roughness->LOD map (envSpecularRoughness,
        // 800.PixelShader_piece_ps.any:4) multiplies by
        // passBuf.envMapNumMipmaps, which stays at its 1.0 default for a plain
        // PBSM_REFLECTION texture — only the PCC classes ever call this — and
        // every reflection is sampled at mip 0-1 however rough the surface.
        // The count only ever grows, so it is reset first.
        Ogre::HlmsPbs *pbs = static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        pbs->resetIblSpecMipmap(0u);
        pbs->_notifyIblSpecMipmap(next->getNumMipmaps());
        applyReflectionToAll();
    }
    if (mSkyShFresh) {
        mSkyShFresh = false;
        std::memcpy(mSkyShInForce, mSkySh, sizeof mSkyShInForce);
        mSkyShInForceValid = true;
        applySkyAmbient(GiStaleReason::Sky);   // the sky's edit, not the light's
    }
}

void OgreScene::destroyPendingReflection() {
    mIblPending = false;
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    if (mIblSourceTex && mIblSourceOwned) {
        try { destroyRecycled(tm, mIblSourceTex); } catch (...) {}
    }
    mIblSourceTex = nullptr;
    mIblSourceOwned = false;
    if (mReflPendingTex) {
        try { destroyRecycled(tm, mReflPendingTex); } catch (...) {}
        mReflPendingTex = nullptr;
    }
}

// ONE CAPTURE RENDER, THREE CALLERS (CLOUDS-2D-1 made it a function): the
// environment capture below, the clear-sky capture the cloud layer is lit by,
// and the export's equirect bake. It renders the capture workspace — render
// queue 0 at visibility 0x1, i.e. the sky and whatever the scene draws over it
// at that queue — from a camera at the origin into a new `size`^2 RGBA16F cube
// named from the recycled pool, and returns it (the caller frees it with
// destroyRecycled). Throws through Ogre's exceptions; the callers catch.
Ogre::TextureGpu *OgreScene::renderSkyCaptureCube(const char *prefix, Ogre::uint32 size, bool mips) {
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    Ogre::TextureGpu *cube = tm->createTexture(
        recycledName(prefix), Ogre::GpuPageOutStrategy::Discard,
        // RenderToTexture because the compositor draws into it;
        // AllowAutomipmaps because BOTH readers of the environment capture want
        // a chain — the SH from the 32^2 level, the ibl_specular pass from all
        // of them (it refuses an input that cannot generate one).
        mips ? (Ogre::TextureFlags::RenderToTexture | Ogre::TextureFlags::AllowAutomipmaps)
             : Ogre::TextureFlags::RenderToTexture,
        Ogre::TextureTypes::TypeCube);
    Ogre::CompositorWorkspace *ws = nullptr;
    try {
        cube->setResolution(size, size, 6u);
        // FLOAT16, NOT sRGB8, and it is the ambient integral that decides it:
        // one 8-bit sRGB step at mid-grey is ~5e-3 of linear radiance, and the
        // rounding a GPU does on that encode is vendor-dependent — so a
        // band-0 assertion against `linearOf(the picked colour)` could only be
        // held to 4e-3, three times looser than the CPU path it replaced. In
        // half-float the capture stores what the shader computed, and the
        // tolerance goes back to 1e-3. It also stops the environment clipping
        // at 1.0, which an HDR sky (a sunset, a bright HDRI) very much does.
        cube->setPixelFormat(Ogre::PFG_RGBA16_FLOAT);
        if (mips) cube->setNumMipmaps(Ogre::PixelFormatGpuUtils::getMaxMipmapCount(size, size));
        cube->scheduleTransitionTo(Ogre::GpuResidency::Resident);

        if (!mIblCamera) mIblCamera = mSceneMgr->createCamera(processUniqueName("iblcam"), false);
        // The capture camera IS the cube's centre: at the origin, unrotated
        // (camera_cubemap_reorient multiplies the six face rotations onto
        // whatever orientation it finds), square, 90 degrees.
        mIblCamera->setPosition(Ogre::Vector3::ZERO);
        mIblCamera->setOrientation(Ogre::Quaternion::IDENTITY);
        mIblCamera->setAspectRatio(1.0f);
        mIblCamera->setFOVy(Ogre::Degree(90.0f));
        Ogre::CompositorChannelVec externals;
        externals.push_back(cube);
        ws = cm->addWorkspace(mSceneMgr, externals, mIblCamera,
                              Ogre::IdString(kSkyCaptureWorkspace), false);
        ws->_beginUpdate(false);
        ws->_update();
        ws->_endUpdate(false);
        cm->removeWorkspace(ws);
        return cube;
    } catch (...) {
        if (ws) { try { cm->removeWorkspace(ws); } catch (...) {} }
        try { destroyRecycled(tm, cube); } catch (...) {}
        throw;
    }
}

// Six render_scene passes over render queue 0 into the six faces of a small
// cube, then two readers of that cube: the ambient SH integral (its 32^2 mip)
// and — for every sky that is not already a cubemap — the GGX convolution that
// produces what every PBR datablock samples.
//
// WHY A SCENE PASS AND NOT SIX QUADS OF OUR OWN. The sky is whatever is bound:
// Ogre's equirect material, Ogre's cube material, or the atmosphere's shader.
// Rendering the SCENE's sky queue means the environment is by construction the
// picture the viewport shows — there is no second implementation of the sky to
// keep in step, which is exactly what the CPU resample was and why a sky change
// had to touch three code paths. The cost is six culls of a queue with one
// object in it, once per sky CHANGE.
//
// WHERE THIS IS CALLED FROM, and why it is not beside applyPendingIbl (which it
// feeds): this is a SCENE pass, so it needs a scene graph that has been updated
// THIS FRAME. `updateSceneGraph` runs after the pending-work loop at the top of
// OgreEngine::renderOneFrame, so a capture there renders a scene whose static
// memory manager has never been walked — and on the first frame of a process
// that means the sky quad has no world AABB yet, culls out, and the capture
// comes back BLACK (measured: the first colour sky of a run integrated to 0,0,0
// and every later one was exact). So the capture runs right after
// updateSceneGraph/applyShadowCacheDirties, still inside the frame; its
// convolution runs right behind it, before the frame draws (applyPendingIbl).
void OgreScene::applyPendingSkyCapture() {
    // THE CLOUD FIELD FIRST (CLOUDS-2D-1): it is a render pass too, and a
    // change that re-bakes it also re-captures — the capture must photograph
    // the new field, not the old one.
    if (mCloudFieldPending) bakeCloudField();
    // ...and the clear sky the sheet is lit by, before the capture that
    // photographs the lit sheet.
    if (mCloudClearPending && mSkyCapturePending && cloudLayerDrawn()) captureCloudClearSky();
    if (!mSkyCapturePending) return;
    mSkyCapturePending = false;
    if (mSkyDesc.mode == SkyMode::NoSky) { forgetSkySh(); return; }
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    if (!cm->hasWorkspaceDefinition(Ogre::IdString(kSkyCaptureWorkspace))) {
        // Media missing (an unstaged tree): no environment rather than a wrong
        // one, and one line saying which file.
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: " + std::string(kSkyCaptureWorkspace) +
            " not found — the sky lights nothing and reflects nothing");
        forgetSkySh();
        landEnvironmentIfComplete();   // a host-pushed cube waiting on this SH
        return;
    }
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    Ogre::TextureGpu *cube = nullptr;
    JAH_TRY {
        cube = renderSkyCaptureCube("skycapture", kSkyCaptureSize, true);

        // THE AMBIENT, off the sky the capture just took. The SUN DISC is
        // deliberately NOT in it (the compositor's queue range), and not in the
        // reflections either: see the note on SunDisc::inProbes in Types.h for
        // what that switch can and cannot reach today.
        integrateSkyShFromCube(cube);

        // WHO OWNS THE ENVIRONMENT. Two descriptions say "not the capture":
        //   * a CUBEMAP sky — its own faces are already the cube, at their full
        //     resolution, and a 128^2 capture would throw detail away;
        //   * a description that carries EXPLICIT reflectionFaces — the host
        //     has stated what the environment is, and SkyDesc says that is what
        //     a datablock samples. A visible sky and a reflected environment are
        //     allowed to differ (gi.probe_open's two-toned sky rests on it), and
        //     the capture must not quietly overrule the host.
        // In both cases the capture still ran, because the AMBIENT is the sky's
        // own light either way.
        if (mSkyDesc.mode == SkyMode::Cubemap || mSkyDesc.reflections) {
            destroyRecycled(tm, cube);
        } else {
            // ...and for every other sky the capture IS the environment. It
            // is convolved HERE, in the capture's own frame and before any draw
            // (we are inside the frame: a command buffer exists), into the next
            // set's cube; the convolution frees the capture afterwards. It used
            // to be queued for the top of the next frame with the newborn cube
            // already bound — the change frame's flash (landEnvironmentIfComplete).
            buildReflectionCubemapFrom(cube, true);
            applyPendingIbl();
        }
        // The whole set lands now when the SH read was synchronous; a deferred
        // read lands it at the next frame's top (readSkyShTicket).
        landEnvironmentIfComplete();
        return;
    } catch (Ogre::Exception &e) {
        mError = e.getFullDescription();
    } catch (std::exception &e) {
        mError = std::string("engine: ") + e.what();
    }
    Ogre::LogManager::getSingleton().logMessage("Jahshaka: sky capture failed: " + mError);
    if (cube) { try { destroyRecycled(tm, cube); } catch (...) {} }
    forgetSkySh();
    landEnvironmentIfComplete();
}

// The ambient, read off the captured cube's 32^2 mip: 6 x 1024 texels, once per
// sky change. It replaces an integral the host ran over the sky's FULL equirect
// image (a 4K HDRI is 8.4 M texels x 9 bands on the UI thread, bounded to 256
// wide by LIGHTS-2 and now not run at all).
//
// THE READBACK RULES, both learned the hard way and both load-bearing:
// `_autogenerateMipmaps` only RECORDS its blits, and an AsyncTextureTicket
// issued before the command buffer is submitted reads the allocation's PREVIOUS
// contents — not zeros, a destroyed texture's pixels. flushCommands() submits.
//
// ...WHICH IS WHY THE FIRST ANSWER IS SYNCHRONOUS AND EVERY LATER ONE IS NOT
// (render audit 2026-09-17 ON-14, lane ENGINE-SMALL-A). `flushCommands()` +
// `map()` is a GPU->CPU WAIT ON THE UI THREAD — it submits everything recorded
// so far in the frame and blocks until the copy of the capture has executed —
// and it ran on every sky CHANGE, i.e. on every frame of a sun drag. Measured
// on this box (Debug engine, Xvfb, 43 sky changes): 0.94 ms mean in the flush +
// download, 0.47 ms in the integral itself, 1.41 ms total per change, and the
// flush's share is unbounded in principle because it waits for whatever the
// frame had already recorded.
//
// The fix is not to move the integral to the GPU (the audit's first idea): the
// 27 coefficients are a CPU CONTRACT — `Engine::skyAmbientSh`, which
// SceneMirror reads EVERY frame and multiplies by the Sky Light's intensity and
// tint — so a compute projection would still have to come back across the bus,
// and what it would save is the 0.47 ms of arithmetic, not the wait. The fix is
// to stop FLUSHING AND WAITING INSIDE THE CAPTURE FRAME: the download is issued
// with inaccurate tracking (no fence, no driver overhead, no mid-frame submit)
// and read at the TOP OF THE NEXT FRAME this scene is drawn in, where the copy
// has had a whole frame of GPU time and the map returns what is already there
// (measured 0.002 ms mean, 0.004 worst, against the 0.94 ms of flush and wait).
// The previous coefficients stay valid meanwhile, so nothing flickers, and the
// LATENCY IS EXACTLY THE ONE FRAME the contract already documents for this and
// for the IBL convolution — see pollSkyShRead for why that promise is worth a
// near-free wait rather than "whenever the transfer lands".
//
// AND A LONE SKY CHANGE STAYS SYNCHRONOUS. This is the rule, and it is chosen
// rather than "always defer" on the measurement plus one behavioural fact:
//
//   * ONE wait is not the hazard. A dial tweak, a sky-mode switch, a project
//     open pays 1.4 ms once and every host, thumbnail, preview and pixel suite
//     sees the ambient exactly when it has always seen it — one frame after the
//     sky, which is what the contract says and what the suites are written
//     against (deferring a lone change costs one MORE frame, and
//     mirror.document_to_engine's red-sky case, which pushes a sky and renders
//     three frames, reads the previous sky's light instead).
//   * A DRAG is the hazard: a sun being dragged captures on EVERY frame, and
//     that is where 1.4 ms of flush and wait per frame is a third of a 60 Hz
//     budget spent doing nothing. From the second consecutive capture on, the
//     read is deferred to the next frame's top and the ambient simply trails
//     the sky by one more frame for as long as the gesture lasts.
//
// `mSkyCaptureIdleFrames` is how many drawn frames have passed since the last
// capture, so "consecutive" is a property of the gesture and not of a timer.
void OgreScene::integrateSkyShFromCube(Ogre::TextureGpu *cube) {
    // A READ FROM THE PREVIOUS FRAME IS FREE NOW: its copy was submitted with
    // that frame and has had one whole frame of GPU time, so this is a map, not
    // a wait — and doing it here rather than dropping the ticket is what keeps
    // the ambient MOVING through a drag (every frame of which would otherwise
    // cancel the read the frame before it issued).
    if (mSkyShTicket) readSkyShTicket(true);
    const bool consecutive = mSkyCaptureIdleFrames <= kSkyCaptureDragFrames;
    mSkyCaptureIdleFrames = 0;
    // The run-wide diagnostic latch every measurable rule in this engine
    // carries (JAHSHAKA_NO_LOD_HYSTERESIS, JAHSHAKA_NO_RAY_QUERY): with it set
    // every capture takes the synchronous path, so the A/B is a run of the
    // shipped binary and not a build.
    static const bool forceSync = std::getenv("JAHSHAKA_SKY_SH_SYNC") != nullptr;
    // A capture the cloud layer's SCROLL asked for (tickCloudClock) is not a
    // gesture but it is PERIODIC, and its ambient trailing by one more frame
    // is invisible, so it takes the no-wait read too: the cadence costs the
    // capture's GPU work and never a GPU->CPU stall on the UI thread.
    const bool asyncOnce = mSkyCaptureAsyncOnce;
    mSkyCaptureAsyncOnce = false;
    if (mSkyShValid && (consecutive || asyncOnce) && !forceSync) { issueSkyShRead(cube); return; }
    integrateSkyShNow(cube);
}

// THE ASYNCHRONOUS ISSUE: record the copy, keep the ticket, wait for nothing.
// No `flushCommands()` — the frame's own commit submits it, and the pin flushes
// the copy encoder itself if the cube is destroyed with a download pending
// (VulkanQueue::notifyTextureDestroyed), which is what makes the capture's
// ordinary lifetime (freed by its convolution in the capture's own frame, or
// straight away for a cubemap sky) safe to leave exactly as it was.
void OgreScene::issueSkyShRead(Ogre::TextureGpu *cube) {
    destroySkyShTicket();       // never overwrite one: the ticket owns a staging buffer
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    JAH_TRY {
        cube->_autogenerateMipmaps();
        const Ogre::uint8 mip = std::min<Ogre::uint8>(kSkyShMip, Ogre::uint8(cube->getNumMipmaps() - 1u));
        const Ogre::uint32 n = std::max(1u, kSkyCaptureSize >> mip);
        mSkyShTicket = tm->createAsyncTextureTicket(n, n, 6u, Ogre::TextureTypes::TypeCube,
                                                    cube->getPixelFormat());
        // accurateTracking FALSE: no fence, and `queryIsTransferDone` answers
        // off the frame counter. It must not be polled in the frame it was
        // issued in (the pin logs a warning and switches to a fence, which
        // would flush) — and it never is: the poll runs at the top of a frame,
        // this issue happens inside one.
        mSkyShTicket->download(cube, mip, false);
        return;
    } JAH_CATCH(mError, );
    destroySkyShTicket();
}

// THE POLL, at the top of every frame of a scene being drawn. Non-blocking by
// contract: `queryIsTransferDone` with inaccurate tracking is a frame-counter
// comparison.
void OgreScene::pollSkyShRead() {
    if (mSkyCaptureIdleFrames < 1000u) ++mSkyCaptureIdleFrames;   // the gesture's clock
    readSkyShTicket(false);
    readCloudClearTicket(false);
}

// READ THE DEFERRED DOWNLOAD, or leave it for the next frame.
//
// `force` false — the frame's top — never waits: `queryIsTransferDone` with
// inaccurate tracking is a frame-counter comparison, and a transfer that has not
// landed is simply tried again next frame. The ticket is KEPT in that case, and
// destroyed on every other exit.
//
// `force` true — a new capture is about to replace it — maps unconditionally,
// and that map is not a stall either: the download it is waiting for was
// recorded in an EARLIER frame and submitted with that frame's own commit, so
// the GPU has had a whole frame to do a 24 KB copy. MEASURED: over a 40-step sun
// drag this path NEVER FIRED — every read had already landed at the frame top
// before the next capture came — and the frame-top maps it stands in for cost
// 0.002 ms mean, 0.004 worst (the integral that follows is 0.46 ms of CPU and is
// the same work in both paths). It is here so that a scene capturing faster than
// its copies land still TAKES the answer instead of dropping it: a dropped read
// would freeze the sky's light for a whole gesture, because every frame of a
// drag would cancel the read the frame before had issued.
void OgreScene::readSkyShTicket(bool force) {
    if (!mSkyShTicket) return;
    JAH_TRY {
        if (!force && !mSkyShTicket->queryIsTransferDone()) return;   // next frame
        const Ogre::TextureBox box = mSkyShTicket->map(0);
        integrateSkyShFromBox(box);
        mSkyShTicket->unmap();
        mSkyShValid = true;
        mSkyShFresh = true;
    } JAH_CATCH(mError, );
    destroySkyShTicket();
    // The deferred read was the set's last part: it lands with its cube.
    landEnvironmentIfComplete();
}

void OgreScene::destroySkyShTicket() {
    if (!mSkyShTicket) return;
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    JAH_TRY {
        if (tm) tm->destroyAsyncTextureTicket(mSkyShTicket);
    } JAH_CATCH(mError, );
    mSkyShTicket = nullptr;
}

// The synchronous form — the first capture of a scene, and the oracle the
// asynchronous one is measured against (JAHSHAKA_SKY_SH_SYNC forces it for
// every capture, which is how the two are A/B'd on one binary).
void OgreScene::integrateSkyShNow(Ogre::TextureGpu *cube) {
    mSkyShValid = false;
    mSkyShFresh = false;
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    Ogre::AsyncTextureTicket *ticket = nullptr;
    JAH_TRY {
        cube->_autogenerateMipmaps();
        mRoot->getRenderSystem()->flushCommands();
        const Ogre::uint8 mip = std::min<Ogre::uint8>(kSkyShMip, Ogre::uint8(cube->getNumMipmaps() - 1u));
        const Ogre::uint32 n = std::max(1u, kSkyCaptureSize >> mip);
        ticket = tm->createAsyncTextureTicket(n, n, 6u, Ogre::TextureTypes::TypeCube,
                                              cube->getPixelFormat());
        ticket->download(cube, mip, true);
        const Ogre::TextureBox box = ticket->map(0);
        integrateSkyShFromBox(box);
        ticket->unmap();
        tm->destroyAsyncTextureTicket(ticket);
        ticket = nullptr;
        mSkyShValid = true;
        mSkyShFresh = true;
        return;
    } catch (Ogre::Exception &e) {
        mError = e.getFullDescription();
    } catch (std::exception &e) {
        mError = std::string("engine: ") + e.what();
    }
    if (ticket) { try { tm->destroyAsyncTextureTicket(ticket); } catch (...) {} }
    Ogre::LogManager::getSingleton().logMessage("Jahshaka: sky ambient integral failed: " + mError);
}

// ONE INTEGRAL, TWO CALLERS: the cube face's texels -> the nine coefficients.
// Neither path may have its own copy of this — the synchronous form is the
// oracle for the asynchronous one, and two implementations could not be.
void OgreScene::integrateSkyShFromBox(const Ogre::TextureBox &box, float *out) {
    const Ogre::uint32 n = box.width;
    {
        ShAccum acc;
        for (int f = 0; f < 6; ++f) {
            const float *fw = kFaceFwd[f], *rt = kFaceRight[f], *up = kFaceUp[f];
            // A cube-face texel's solid angle is (2/N)(2/N) / |dir|^3 before
            // normalisation — the projection of the flat face onto the sphere.
            const double texel = (2.0 / n) * (2.0 / n);
            for (Ogre::uint32 py = 0; py < n; ++py) {
                const float ny = 1.0f - 2.0f * (py + 0.5f) / n;
                const Ogre::uint16 *row = reinterpret_cast<const Ogre::uint16 *>(box.at(0, py, size_t(f)));
                for (Ogre::uint32 px = 0; px < n; ++px) {
                    const float nx = 2.0f * (px + 0.5f) / n - 1.0f;
                    float dx = fw[0] + rt[0] * nx + up[0] * ny;
                    float dy = fw[1] + rt[1] * nx + up[1] * ny;
                    float dz = fw[2] + rt[2] * nx + up[2] * ny;
                    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (len < 1e-6f) continue;
                    const double w = texel / (double(len) * len * len);
                    dx /= len; dy /= len; dz /= len;
                    // Half floats, and ALREADY LINEAR: the capture target is
                    // RGBA16_FLOAT, so what the sky's shader computed is what
                    // is stored — no sRGB decode, no 8-bit step.
                    const Ogre::uint16 *t = row + size_t(px) * 4u;
                    acc.add(dx, dy, dz, Ogre::Bitwise::halfToFloat(t[0]),
                            Ogre::Bitwise::halfToFloat(t[1]),
                            Ogre::Bitwise::halfToFloat(t[2]), w);
                }
            }
        }
        acc.finish(out ? out : mSkySh);
    }
}

bool OgreScene::applySkyReflectionFaces(const TextureId faces[6]) {
    JAH_TRY {
        bool anySet = false;
        for (int i = 0; i < 6; ++i) if (faces[i]) anySet = true;
        if (!anySet) { destroyReflection(); return true; }
        Ogre::TextureGpu *tex[6];
        for (int i = 0; i < 6; ++i) {
            auto it = mTextures.find(faces[i]);
            if (it == mTextures.end()) { mError = "setSky: unknown reflection face texture"; return false; }
            tex[i] = it->second.texture;
        }
        // Same reason as the cubemap path: read, not bound.
        for (int i = 0; i < 6; ++i) waitForTextureResident(tex[i]);
        for (int i = 0; i < 6; ++i)
            if (tex[i]->getWidth() != tex[0]->getWidth() || tex[i]->getHeight() != tex[0]->getWidth()) {
                mError = "setSky: the six reflection faces must be square and the same size";
                return false;
            }
        // The convolution INPUT: the world->Ogre cube, with AllowAutomipmaps
        // because CompositorPassIblSpecular generates the input's mip chain
        // itself and refuses an input that cannot
        // (OgreCompositorPassIblSpecular.cpp:128).
        Ogre::TextureGpu *cube = buildCubeFromWorldFaces(
            tex, "skyiblsrc",
            Ogre::TextureFlags::RenderToTexture | Ogre::TextureFlags::AllowAutomipmaps, true);
        if (!cube) return false;   // mError set
        buildReflectionCubemapFrom(cube, true);
        return true;
    } JAH_CATCH(mError, false);
}

// ADDENDUM A-5: a cubemap the HOST owns, from six world-axis faces.
//
// Deliberately the SAME builder the sky's reflection half uses: the backend samples
// cubemaps LEFT-HANDED, so world-axis faces need a face swap plus per-axis
// mirroring (buildCubeFromWorldFaces' table). A second copy of that remap is
// how every reflection in the scene ends up silently mirrored — the 2026-09-03
// finding, from when the sky itself had the bug.
//
// Plain ManualTexture flags and its own mip chain: this cube is SAMPLED, never
// rendered into, so it needs neither RenderToTexture nor the IBL specular
// convolution's AllowAutomipmaps. A shorter mip chain than the global cube's
// samples clamped under `_notifyIblSpecMipmap`'s process-wide count — acceptable
// and noted, not measured.
TextureId OgreScene::createCubemap(const TextureId faces[6]) {
    JAH_TRY {
        Ogre::TextureGpu *tex[6];
        for (int i = 0; i < 6; ++i) {
            auto it = mTextures.find(faces[i]);
            if (it == mTextures.end() || !it->second.texture) {
                mError = "createCubemap: unknown face texture"; return 0;
            }
            tex[i] = it->second.texture;
        }
        for (int i = 0; i < 6; ++i) waitForTextureResident(tex[i]);
        for (int i = 0; i < 6; ++i)
            if (tex[i]->getWidth() != tex[0]->getWidth() ||
                tex[i]->getHeight() != tex[0]->getWidth() ||
                tex[i]->getPixelFormat() != tex[0]->getPixelFormat()) {
                mError = "createCubemap: the six faces must be square, the same size "
                         "and the same format";
                return 0;
            }
        Ogre::TextureGpu *cube = buildCubeFromWorldFaces(tex, "matcube", 0, true);
        if (!cube) return 0;   // mError set
        TextureRec rec;
        rec.texture = cube;
        rec.path = "";      // pixel-born: never deduplicated, the caller owns it
        rec.srgb = false;   // the faces decide; the cube copies their format
        return trackTexture(rec);
    } JAH_CATCH(mError, 0);
}

void OgreScene::tuneSkyRenderable() {
    Ogre::Rectangle2D *sky = mSceneMgr->getSky();
    if (!sky) return;
    // Sky.material clamps BOTH axes. That is right for a cube and wrong for a
    // lat-long image, whose left and right edges are the same meridian: clamping
    // u leaves a seam column where the bilinear filter stops wrapping. Our own
    // sky sphere wrapped u; keep it.
    Ogre::MaterialPtr skyMat = mSceneMgr->getSkyMaterial();
    if (mSceneMgr->getSkyMethod() == Ogre::SceneManager::SkyEquirectangular && skyMat &&
        skyMat->getNumTechniques() > 0 &&
        skyMat->getTechnique(0)->getNumPasses() > 0 &&
        skyMat->getTechnique(0)->getPass(0)->getNumTextureUnitStates() > 0) {
        Ogre::HlmsSamplerblock sampler;
        sampler.setFiltering(Ogre::TFO_TRILINEAR);
        sampler.mU = Ogre::TAM_WRAP;
        sampler.mV = Ogre::TAM_CLAMP;
        sampler.mW = Ogre::TAM_CLAMP;
        skyMat->getTechnique(0)->getPass(0)->getTextureUnitState(0)->setSamplerblock(sampler);
    }
    // Ogre parks the sky at render queue 212 ("render after most stuff"), which
    // is AFTER our on-top overlays (kOverlayRenderQueue) — a gizmo drawn over empty sky would be
    // painted out, because overlays deliberately write no depth. Queue 0 is where
    // our own sky quads used to sit: the sky writes no depth either, so drawing
    // first costs one screen of overdraw and preserves every existing ordering.
    sky->setRenderQueueGroup(0);
    // ...AND AT SUBGROUP 1 WITHIN IT (lane HAM-1). Queue 0 has exactly one
    // other tenant, and only inside a VR eye: the runtime's hidden-area mesh, a
    // depth-only draw at the NEAR plane whose whole purpose is that nothing
    // behind it is ever shaded. The sky's material is `depth_check on,
    // depth_write off` and draws at the FAR plane, so it is masked out by that
    // depth — but only if it is drawn AFTER it, and the subgroup is the top
    // field of the render queue's sort key (OgreRenderQueue::addRenderable), so
    // it is the one ordering guarantee available inside a queue. At equal
    // subgroups the order falls to a material/mesh hash, i.e. to luck.
    //
    // NOTHING ELSE MOVES: items are at queue 10, particles at 15, the sun disc
    // at 5, decals in their own, and the sky quad is the only renderable in
    // queue 0 in every picture this engine draws that has no headset in it — so
    // this line reorders nothing on the desktop (the selftest hash holds) and
    // buys the mask the sky's own fill, which is the largest single thing it
    // saves (an open scene's masked corners are all sky).
    sky->setRenderQueueSubGroup(1u);
    // Instant Radiosity casts rays with mVisibilityMask = kGiGeometryBit; the sky
    // must never be hit by them (nor counted as GI geometry anywhere else).
    sky->setVisibilityFlags(kVisibleBit);
}

Ogre::TextureGpu *OgreScene::makeSkyArrayTexture(Ogre::TextureGpu *src) {
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    // A RECYCLED NAME, like every other texture this file creates (SKYARRAY-LEAK-1;
    // the recycling rule and its cost are at the head of this file and at
    // recycledName() in EnginePrivate.h). This was the one site left on
    // processUniqueName, and it burned a name that can never be used again on
    // EVERY sky application: measured on this tree, one `world.sky(...)` call
    // costs FOUR of them (the scene applies the sky once per push and the
    // copy is remade each time), 181 names in a 60-change script, 138 in a few
    // minutes of the owner's 2026-09-18 smoke — where they finally showed up
    // as "Cannot locate resource skyarray_NN" (ledger §769).
    //
    // WHY A DEAD NAME IS LOUD AT ALL, and why recycling answers it: Ogre's v1
    // TextureUnitState — the sky material's — keeps the texture's NAME beside
    // the pointer, and `notifyTextureChanged(Deleted)` nulls the pointer and
    // KEEPS the name (OgreTextureUnitState.cpp:1347-1364), as does the
    // `_unload` any material reload runs (`cleanFramePtrs`). The next
    // `ensureLoaded` re-resolves that name against the resource GROUPS —
    // `findTextureNoThrow` first — which for a manual texture that no longer
    // exists is a FileNotFoundException, a CRITICAL log line and
    // `mTextureLoadFailed = true` (the layer renders blank from then on).
    // With the name recycled the lookup finds the sky array that is live NOW,
    // which is the texture that TU wants; and the pool stays at one or two
    // slots instead of growing for the life of the process.
    Ogre::TextureGpu *dst = tm->createTexture(
        recycledName("skyarray"), Ogre::GpuPageOutStrategy::Discard,
        Ogre::TextureFlags::ManualTexture, Ogre::TextureTypes::Type2DArray);
    dst->setResolution(src->getWidth(), src->getHeight(), 1u);
    dst->setPixelFormat(src->getPixelFormat());
    dst->setNumMipmaps(1u);
    // Immediate residency and NO explicit notifyDataIsReady: _transitionTo does
    // it itself for a ManualTexture, and a second call underflows the pending
    // counter so isDataReady() never turns true (see createTexture).
    dst->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    // The src's staging upload may only be RECORDED, not submitted — a copy
    // issued now reads recycled VRAM (garbage sky, seen under scripted
    // fixed-dt rendering where no frame flushed in between). Submit first
    // (the AsyncTextureTicket rule from the sky/IBL adoption applies to any
    // dependent GPU read, copies included).
    mRoot->getRenderSystem()->flushCommands();
    src->copyTo(dst, dst->getEmptyBox(0), 0, src->getEmptyBox(0), 0);
    return dst;
}

Ogre::TextureGpu *OgreScene::buildCubeFromWorldFaces(Ogre::TextureGpu *const tex[6],
                                                     const std::string &namePrefix,
                                                     Ogre::uint32 extraFlags, bool mips) {
    const Ogre::uint32 w = tex[0]->getWidth(), h = tex[0]->getHeight();
    const Ogre::PixelFormatGpu pf = tex[0]->getPixelFormat();
    if (Ogre::PixelFormatGpuUtils::isCompressed(pf)) {
        mError = "sky faces must be an uncompressed format";
        return nullptr;
    }
    const size_t bpp = Ogre::PixelFormatGpuUtils::getBytesPerPixel(pf);
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    Ogre::TextureGpu *cube = tm->createTexture(
        recycledName(namePrefix.c_str()), Ogre::GpuPageOutStrategy::Discard,
        Ogre::TextureFlags::ManualTexture | extraFlags, Ogre::TextureTypes::TypeCube);
    cube->setResolution(w, h, 6u);
    cube->setPixelFormat(pf);
    // THE MIP CHAIN MUST BE WRITTEN BY SOMEBODY (2026-09-11, the reflection_map
    // two-state defect). A caller that passes AllowAutomipmaps gets its chain
    // generated later (the sky passes, the IBL convolution); a HOST cube (the
    // per-material reflection override via createCubemap) had a full chain
    // allocated and only mip 0 uploaded — mips 1..N held whatever VRAM the
    // allocator handed back, and HlmsPbs samples a rough reflection at
    // LOD = roughness x envMapNumMipmaps x 1.95, i.e. mostly those unwritten
    // mips: a different garbage picture per allocation history (cold vs warm
    // texture cache), found by scripting.e2e.reflection_map redding in every
    // gate. A host cube now gets its chain box-filtered on the CPU below; a
    // format we cannot filter (not 4 bytes per texel) gets ONE mip — never an
    // unwritten level.
    const bool hostChain = mips && !(extraFlags & Ogre::TextureFlags::AllowAutomipmaps);
    const bool filterable = bpp == 4u;
    const bool withMips = mips && (!hostChain || filterable);
    cube->setNumMipmaps(withMips ? Ogre::PixelFormatGpuUtils::getMaxMipmapCount(w, h) : 1u);
    cube->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    const bool buildChain = hostChain && filterable;
    std::vector<Ogre::uint8> faces;   // the six flipped faces, for the host chain
    if (buildChain) faces.resize(size_t(w) * h * bpp * 6u);

    // ONE staging texture for all six slices, one upload: six separate
    // getStagingTexture/upload/removeStagingTexture rounds inside a single frame
    // left the first slice reading black on the frame after the change.
    // HARD-WON: the six source faces were very likely uploaded moments ago by
    // createTexture, whose staging upload only RECORDS a copy into the open
    // command buffer. An AsyncTextureTicket download issued before that buffer
    // is submitted reads the texture's VRAM as it was BEFORE the copy — and
    // since Ogre recycles freed allocations, "before" is a previous texture's
    // pixels, not zeros. It cost an afternoon: the first cube face came back
    // carrying a destroyed sky image from an earlier test and only that one
    // direction rendered wrong. flushCommands() submits the pending buffer;
    // waitForStreamingCompletion alone does NOT (it drains the streaming worker,
    // which manual uploads never went through).
    tm->waitForStreamingCompletion();
    mRoot->getRenderSystem()->flushCommands();
    Ogre::StagingTexture *staging = tm->getStagingTexture(w, h, 1u, 6u, pf);
    staging->startMapRegion();
    Ogre::TextureBox dst = staging->mapRegion(w, h, 1u, 6u, pf);
    for (int dstFace = 0; dstFace < 6; ++dstFace) {
        Ogre::TextureGpu *srcTex = tex[kSrcFace[dstFace]];
        Ogre::AsyncTextureTicket *ticket = tm->createAsyncTextureTicket(
            w, h, 1u, Ogre::TextureTypes::Type2D, pf);
        ticket->download(srcTex, 0, true);
        const Ogre::TextureBox box = ticket->map(0);
        const bool fh = kFlipH[dstFace], fv = kFlipV[dstFace];
        for (Ogre::uint32 y = 0; y < h; ++y) {
            const Ogre::uint8 *in = reinterpret_cast<const Ogre::uint8 *>(
                box.at(0, fv ? (h - 1u - y) : y, 0));
            Ogre::uint8 *out = reinterpret_cast<Ogre::uint8 *>(dst.at(0, y, size_t(dstFace)));
            if (!fh) {
                std::memcpy(out, in, size_t(w) * bpp);
            } else {
                for (Ogre::uint32 x = 0; x < w; ++x)
                    std::memcpy(out + size_t(x) * bpp, in + size_t(w - 1u - x) * bpp, bpp);
            }
            if (buildChain)
                std::memcpy(&faces[((size_t(dstFace) * h) + y) * size_t(w) * bpp], out, size_t(w) * bpp);
        }
        ticket->unmap();
        tm->destroyAsyncTextureTicket(ticket);
    }
    staging->stopMapRegion();
    staging->upload(dst, cube, 0, nullptr, nullptr, true);
    tm->removeStagingTexture(staging);

    // The host chain: a 2x2 box filter per level, all six faces per upload
    // (one staging texture per level, like mip 0). Values are filtered as
    // stored; for an sRGB format that is a filter in encoded space — the same
    // approximation Ogre's own automipmap blit makes, and far closer to right
    // than an unwritten level.
    if (buildChain) {
        std::vector<Ogre::uint8> next;
        Ogre::uint32 cw = w, ch = h;
        for (Ogre::uint8 mip = 1; mip < cube->getNumMipmaps(); ++mip) {
            const Ogre::uint32 nw = std::max(1u, cw / 2u), nh = std::max(1u, ch / 2u);
            next.assign(size_t(nw) * nh * 4u * 6u, 0);
            for (int f = 0; f < 6; ++f) {
                const Ogre::uint8 *src = &faces[size_t(f) * ch * cw * 4u];
                Ogre::uint8 *dstPx = &next[size_t(f) * nh * nw * 4u];
                for (Ogre::uint32 y = 0; y < nh; ++y) {
                    const Ogre::uint32 y0 = std::min(y * 2u, ch - 1u), y1 = std::min(y * 2u + 1u, ch - 1u);
                    for (Ogre::uint32 x = 0; x < nw; ++x) {
                        const Ogre::uint32 x0 = std::min(x * 2u, cw - 1u), x1 = std::min(x * 2u + 1u, cw - 1u);
                        for (int c = 0; c < 4; ++c) {
                            const unsigned sum = src[(size_t(y0) * cw + x0) * 4u + c] +
                                                 src[(size_t(y0) * cw + x1) * 4u + c] +
                                                 src[(size_t(y1) * cw + x0) * 4u + c] +
                                                 src[(size_t(y1) * cw + x1) * 4u + c];
                            dstPx[(size_t(y) * nw + x) * 4u + c] = Ogre::uint8((sum + 2u) / 4u);
                        }
                    }
                }
            }
            Ogre::StagingTexture *st = tm->getStagingTexture(nw, nh, 1u, 6u, pf);
            st->startMapRegion();
            Ogre::TextureBox box = st->mapRegion(nw, nh, 1u, 6u, pf);
            for (int f = 0; f < 6; ++f)
                for (Ogre::uint32 y = 0; y < nh; ++y)
                    std::memcpy(box.at(0, y, size_t(f)), &next[((size_t(f) * nh) + y) * nw * 4u],
                                size_t(nw) * 4u);
            st->stopMapRegion();
            st->upload(box, cube, mip, nullptr, nullptr, true);
            tm->removeStagingTexture(st);
            faces.swap(next);
            cw = nw; ch = nh;
        }
    }
    return cube;
}

// EVERY sky change lands here: the NEXT environment cube is allocated beside
// the one in force, never in its place (PHOTON-SKY-TRANSIENT-1). The cube in
// force stays bound until landEnvironmentIfComplete swaps the whole set, so no
// datablock ever samples a cube before the convolution has written it. (This
// function used to destroy the bound cube and bind the newborn one straight
// away, with the convolution a frame later: the change frame's flash.)
//
// Ogre identifies a texture by its TextureGpu POINTER in two caches that
// outlive it: VulkanTextureGpuManager::mCachedTex (the image views a
// descriptor set is built from) and DescriptorSetTexture::operator!= (which is
// how bakeTextures decides a datablock's set is unchanged and can be kept). A
// recycled address therefore makes an old, dead view look current — which is
// why the swap destroys the old cube FIRST (its TextureGpuListener::Deleted
// kills the stale sets) and only then binds the new one. Do not "optimise" the
// order in landEnvironmentIfComplete or destroyReflection.
void OgreScene::buildReflectionCubemapFrom(Ogre::TextureGpu *srcCube, bool ownsSource) {
    // An unlanded next set is superseded by this one.
    destroyPendingReflection();
    mIblSourceTex = srcCube;
    mIblSourceOwned = ownsSource;
    const Ogre::uint32 w = srcCube->getWidth(), h = srcCube->getHeight();
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    // The OUTPUT the PBR datablocks will sample: same size, mipped, and a UAV,
    // which is what the compute integrator writes through.
    Ogre::TextureGpu *cube = tm->createTexture(
        recycledName("skyrefl"), Ogre::GpuPageOutStrategy::Discard,
        Ogre::TextureFlags::RenderToTexture | Ogre::TextureFlags::Uav |
            // Reinterpretable: the sky faces are sRGB, and a Vulkan storage image
            // may not be — the integrator binds the UAV through a linear view and
            // DescriptorSetUav::checkValidity refuses without this flag.
            Ogre::TextureFlags::Reinterpretable |
            Ogre::TextureFlags::AllowAutomipmaps,   // the no-compute fallback path
        Ogre::TextureTypes::TypeCube);
    cube->setResolution(w, h, 6u);
    cube->setPixelFormat(srcCube->getPixelFormat());
    cube->setNumMipmaps(Ogre::PixelFormatGpuUtils::getMaxMipmapCount(w, h));
    cube->scheduleTransitionTo(Ogre::GpuResidency::Resident);
    // (No hand-over for sampling here: nothing samples this cube before the
    // convolution has written it and handOverForSampling has moved it to the
    // Texture layout — that is the point of the pending set.)
    mReflPendingTex = cube;
    // The convolution is a compute dispatch: it needs a command buffer, so it
    // runs in applyPendingIbl — straight away when the caller is the capture
    // (inside the frame), at the top of the next frame for a host-pushed cube.
    mIblPending = true;
}

void OgreScene::applyPendingIbl() {
    if (!mIblPending) return;
    mIblPending = false;
    convolvePendingIbl();
    // The convolution may have been the set's last part (a host-pushed cube, or
    // a capture whose SH was read synchronously).
    landEnvironmentIfComplete();
}

void OgreScene::convolvePendingIbl() {
    if (!mIblSourceTex || !mReflPendingTex) return;
    Ogre::TextureGpu *const target = mReflPendingTex;
    // A cube we own is scratch: once the convolution has read it, its (mipped,
    // full-size) VRAM is dead weight until the next sky change.
    struct FreeSource {
        OgreScene *self = nullptr;
        ~FreeSource() {
            if (!self->mIblSourceOwned || !self->mIblSourceTex) return;
            try {
                destroyRecycled(self->mRoot->getRenderSystem()->getTextureGpuManager(),
                                self->mIblSourceTex);
            } catch (...) {}
            self->mIblSourceTex = nullptr;
            self->mIblSourceOwned = false;
        }
    } freeSource{ this };
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    Ogre::CompositorWorkspace *ws = nullptr;
    JAH_TRY {
        if (!cm->hasWorkspaceDefinition(Ogre::IdString(kIblWorkspace))) {
            // JahshakaIbl.compositor was not staged/registered: fall back to the
            // box mip chain (what this engine did before the adoption wave) so a
            // missing media folder degrades quality instead of killing reflections.
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: " + std::string(kIblWorkspace) +
                " not found — sky reflections fall back to box mipmaps");
            mIblSourceTex->copyTo(target, target->getEmptyBox(0), 0,
                                  mIblSourceTex->getEmptyBox(0), 0);
            target->_autogenerateMipmaps();
            handOverForSampling(mRoot, target);
            return;
        }
        if (!mIblCamera) mIblCamera = mSceneMgr->createCamera(processUniqueName("iblcam"), false);
        Ogre::CompositorChannelVec externals;
        externals.push_back(mIblSourceTex);
        externals.push_back(target);
        ws = cm->addWorkspace(mSceneMgr, externals, mIblCamera,
                              Ogre::IdString(kIblWorkspace), false);
        ws->_beginUpdate(false);
        ws->_update();
        ws->_endUpdate(false);
        cm->removeWorkspace(ws);
        // THE CONVOLUTION LEFT IT A UAV, AND EVERY DATABLOCK WILL SAMPLE IT
        // (ENVPROBE-LAYOUT-1): `CompositorPassIblSpecular::analyzeBarriers`
        // resolves the output to `ResourceLayout::Uav` and this workspace has
        // no later pass to move it back, so without this the cube is sampled in
        // `VK_IMAGE_LAYOUT_GENERAL` for the rest of its life. See
        // handOverForSampling.
        handOverForSampling(mRoot, target);
        return;
    } catch (Ogre::Exception &e) {
        mError = e.getFullDescription();
    } catch (std::exception &e) {
        mError = std::string("engine: ") + e.what();
    }
    // The convolution failed (a driver without compute, a format that cannot be
    // a UAV): say so loudly and fall back to the box mip chain this engine used
    // before, rather than silently shipping a cubemap nothing ever wrote to.
    Ogre::LogManager::getSingleton().logMessage("Jahshaka: sky IBL specular failed: " + mError);
    if (ws) { try { cm->removeWorkspace(ws); } catch (...) {} }
    JAH_TRY {
        mIblSourceTex->copyTo(target, target->getEmptyBox(0), 0,
                              mIblSourceTex->getEmptyBox(0), 0);
        target->_autogenerateMipmaps();
        handOverForSampling(mRoot, target);
    } JAH_CATCH(mError, );
}

void OgreScene::destroyReflection() {
    destroyPendingReflection();
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    if (!mReflectionTex) return;
    Ogre::TextureGpu *tex = mReflectionTex;
    mReflectionTex = nullptr;
    // DESTROY FIRST, UNBIND SECOND. The order is load-bearing and the reverse
    // was an intermittent SIGSEGV inside the graphics driver, in
    // vkUpdateDescriptorSets (2026-09-03; it surfaced as "editor.screenshot with
    // post-fx crashes on scenes with mirrors", because a planar reflection and
    // an offscreen shot are each an extra scene render re-binding these sets).
    //
    // destroyTexture() fires TextureGpuListener::Deleted, and
    // HlmsTextureBaseClass::notifyTextureChanged's handler both nulls the slot
    // AND destroys the datablock's mTexturesDescSet on the spot — which is the
    // ONLY thing that releases the Vulkan image view Ogre cached for this
    // TextureGpu*. Unbinding first (setTexture(PBSM_REFLECTION, nullptr)) removes
    // the datablock from the texture's listener list, so Deleted reaches nobody:
    // the stale set survives, its view outlives the image, and — because the
    // replacement cubemap usually lands on the freed address (see
    // buildReflectionCubemapFrom) — bakeTextures then compares the two sets
    // EQUAL and never rebuilds it. Every later draw binds a dead view.
    //
    // applyReflectionToAll() still runs afterwards: it is the belt-and-braces
    // unbind for any datablock in mMaterials that was not listening, and the
    // rebind to the new cubemap when a caller follows this with a rebuild.
    // Not JAH_CATCH: that returns, and the unbind below must happen even if the
    // destroy throws (a double-destroy would otherwise leave every datablock
    // pointing at the old cubemap).
    try { destroyRecycled(tm, tex); }
    catch (Ogre::Exception &e)  { mError = e.getFullDescription(); }
    catch (std::exception &e)   { mError = std::string("engine: ") + e.what(); }
    applyReflectionToAll();
    // Recompute envMapNumMipmaps from whatever reflection textures remain
    // (_notifyIblSpecMipmap only ever grows it).
    static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS))
        ->resetIblSpecMipmap(0u);
}

void OgreScene::applyReflectionToAll() { applyReflectionToAllImpl(); }

// THE ENV-PROBE SLOT HAS ONE OCCUPANT (found the hard way, 2026-09-07, the
// reflections P3/P6 lane; it is why `probeGridBound()` gates `reflectionTexFor`
// rather than every site just reading mReflectionTex).
//
// The PBS pixel shader has exactly ONE env-probe texture, `texEnvProbeMap`. An
// automatic ParallaxCorrectedCubemap — which is what the VCT+PCC hybrid builds —
// fills it from the PASS with a cube ARRAY of probes. A datablock that also
// carries its own PBSM_REFLECTION cubemap makes HlmsPbs's `canUseManualProbe`
// true, which SUPPRESSES `use_parallax_correct_cubemaps` while the pass property
// `hlms_enable_cubemaps_auto` stays set, and the generated shader then fails to
// compile in three separate places (measured, in this order, each one revealed
// by fixing the one before it):
//   * `toProbeLocalSpace` / `localCorrect` — declared only under
//     use_parallax_correct_cubemaps, called by the auto path;
//   * `vctSpecPosVS` — declared and passed to computeVctProbe under the same
//     property, read by the auto path's getPccVctBlendWeight;
//   * `SampleEnvProbe` in CubemapGlobal — no OGRE_SampleLevelF16 overload takes
//     a textureCubeArray.
// The third one is the one that decides the fix: the manual cubemap is not
// merely undeclared in that permutation, it is UNSAMPLEABLE, because the slot
// holds an array. Upstream cannot serve both and no patch of ours would change
// that; the two are mutually exclusive by construction in this pin.
//
// So while auto PCC is bound WE do not bind the IBL cubemap. That much is
// unchanged and cannot change: the two are mutually exclusive in this pin.
//
// Before this, picking VCT+Probes on any scene with a sky produced a shader that
// did not compile — i.e. objects that did not draw at all — and it was invisible
// to every suite because no suite combined the two. `gi.pcc_mirror`'s sky case
// is the fence; it goes black without this.
//
// WHAT CHANGED IS WHERE THE SKY WENT INSTEAD (lane SKY-FALLBACK-1,
// ogre-patch 0048). The old note said "nothing is lost visually: the probe
// captures include the sky, so the probes ARE the environment". That was true
// while a probe grid was an all-or-nothing scene-wide decision — a grid meant a
// room, and in a room the probes are the environment. It stopped being true the
// day the grid became a PER PROBE decision (lane R5-ROOM): a PARTIAL grid is
// the normal case now, one crate in a new project keeps a handful of its
// candidates, and every pixel that no surviving probe's box contains had NO
// environment left at all. Measured as a bar: e2e_default_ground's grazing
// specular margin fell from 5/255 to 3/255 the day that landed.
//
// So the sky has its OWN slot now, at the pass level, through the extra
// pass texture HlmsPbs offers its Hlms listener
// (FogHlmsListener::SkyEnvState / getNumExtraPassTextures / hlmsTypeChanged,
// OgreFog.cpp; the composite is ogre-patch 0048, inside upstream's per-pixel
// probe loop). It is bound for every colour pass of a scene whose grid has
// taken the env slot, and the probe loop hands it every pixel no probe's box
// contains. This function therefore still returns null under a PCC — the slot
// still has one occupant — and the sky is no longer lost by it.
//
// rebuildVct/teardownVct call applyReflectionToAll() so both bindings follow the
// hybrid up and down (the pass-level one is pushed from refreshEnvmapScale,
// which that call funnels through).
//
// THE RESIDUAL, recorded rather than fixed here: an AUTHORED reflection map on a
// material is still unbound under a PCC, and the pass-level slot carries the
// SKY, not that map. A material with its own environment therefore loses it
// while a grid exists, exactly as before. Closing that needs a per-datablock
// environment texture, which this pin does not have.
//
// THE QUESTION IS THIS SCENE'S (PHOTON-SCENE-SWITCH-1). `HlmsPbs` sets
// `parallax_correct_cubemaps` — and therefore makes `texEnvProbeMap` a cube ARRAY
// — for every pass that runs with a PCC bound (OgreHlmsPbs.cpp:1820-1828), and
// since the binding became per pass (SceneGiBinding) only THIS scene's passes run
// with this scene's grid. So the answer is "does this scene's binding hold a
// grid": its datablocks drop their manual cube when it does, and no other
// scene's are touched. (While the binding was process-wide the question was too,
// and every grid transition walked every scene — a preview whose materials kept
// their sky cube generated `SampleEnvProbe` against a cube array and drew
// nothing: the avatar preview's black character, r3 g3 b4. gi.pcc_second_scene
// guards both halves.) The scene keeps its sky either way: under a grid its own
// sky cube reaches its materials through the pass-level slot below (the state is
// per SceneManager — FogHlmsListener::SkyEnvState).
//
// THE ROUGHNESS-TO-LOD MAP AFTER A PROBE TRANSITION (lane SKY-FALLBACK-1,
// second read). `passBuf.envMapNumMipmaps` is ONE number for the whole pass and
// `_notifyIblSpecMipmap` only ever GROWS it.
// `ParallaxCorrectedCubemapAuto::setEnabled` pushes the PROBE ARRAY's count into
// it (6 while the placement holds the scout's 32 px, 10 at a 512 px tier) and
// the engine pushed the SKY cube's count only when the cube was BUILT, never
// again — so once a grid had come and gone, every manual cube in every scene
// mapped its roughness against a chain it does not have. The scene-wide walk
// this lane added would spread one scene's transition to all of them.
//
// SO IT RUNS ON THIS SCENE'S TRANSITION AND NOWHERE ELSE
// (noteProbeGridBindingChanged), which is the whole of the fix and was measured
// the hard way. Putting it inside applyReflectionToAllImpl —
// which every sky build, gain edge and material edit funnels through — changes
// scenes that never had a grid at all: `scripting.e2e.ssr_mirror`'s "SSR is
// still in this picture" bar fell 11 -> 7 against a bar of 8, reproducibly at
// -j2 and in BOTH forms (the growth-only notify and the stricter
// `resetIblSpecMipmap(0)` re-derivation), because a blurrier environment term is
// a smaller difference between SSR on and off. That is a real picture question
// about scenes with an authored cube and no probes, it is not this lane's, and
// it is recorded for the lead rather than absorbed by widening somebody's bar.
void OgreScene::renotifyReflectionMipmaps() {
    JAH_TRY {
        auto *hlmsPbs = static_cast<Ogre::HlmsPbs *>(
            mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        if (!hlmsPbs) return;
        unsigned mips = 0;
        for (const auto &kv : mMaterials) {
            if (kv.second.unlit) continue;
            if (Ogre::TextureGpu *bound = reflectionTexFor(kv.second))
                mips = std::max(mips, unsigned(bound->getNumMipmaps()));
        }
        if (mips > 1u) hlmsPbs->_notifyIblSpecMipmap(Ogre::uint8(mips));
    } JAH_CATCH(mError, );
}

void OgreScene::noteProbeGridBindingChanged() {
    applyReflectionToAll();
    renotifyReflectionMipmaps();
}

void OgreScene::applyReflectionToAllImpl() {
    // THE ENV SLOT'S OCCUPANT JUST CHANGED, AND SO DID ITS SCALE. This runs on
    // every PCC bind/unbind (rebuildVct/teardownVct call it for exactly that
    // reason), and the Sky Light's gain applies to the SKY CUBE and not to a
    // probe capture — see envmapScaleForPass(). The gain is written into the
    // ambient pass data, which nothing else here touches, so it has to be
    // re-written here or a scene that acquired its probes after its ambient
    // keeps the sky-cube answer.
    refreshEnvmapScale();
    auto *hlmsPbs = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS);
    for (auto &kv : mMaterials) {
        if (kv.second.unlit) continue;
        auto *db = static_cast<Ogre::HlmsPbsDatablock *>(hlmsPbs->getDatablock(Ogre::IdString(kv.second.datablockName)));
        // PER MATERIAL now (ADDENDUM A-5): a material with its own reflection
        // cubemap keeps it here, and one without gets the global IBL cube —
        // and BOTH go dark under automatic PCC, because reflectionTexFor
        // carries that gate. This loop used to compute one answer for the whole
        // scene, which is exactly what an override cannot survive.
        if (db) db->setTexture(Ogre::PBSM_REFLECTION, reflectionTexFor(kv.second));
    }
}

// ---------------------------------------------------------------------------
// THE SUN DISC (SPECS/SKY_LIGHT_SPEC.md §3, owner decision D15)
// ---------------------------------------------------------------------------
// ONE mechanism, owned by the sun light, drawn over every sky type. A second
// Rectangle2D beside Ogre's own sky quad, at render queue 1 — after the sky
// (queue 0) and long before opaque geometry, so the scene occludes it through
// the depth test exactly as it occludes the sky.
//
// WHY A QUAD AND NOT A BILLBOARD OR A SPHERE. The disc is at INFINITY: it has a
// direction and an angular size and no position at all, which is precisely what
// a full-screen quad with the camera's ray per pixel expresses (Ogre hands
// `Ogre/Compositor/QuadCameraDirNoUV_vs` the corner rays of whichever camera is
// rendering, so one quad is correct in every view of the scene — the editor,
// the player, a thumbnail, a probe face — with no per-camera work of ours).
//
// WHY NOT AN Ogre PATCH. It is not a variant of the sky material: it composes
// OVER any of them, the uniform strip a colour sky bakes included. The material
// is ours, in our own media folder, and upstream is untouched.
void OgreScene::applySunDisc(const SunDisc &sun) {
    JAH_TRY {
        if (!sun.enabled) {
            // Hidden, not destroyed: the sun is switched on and off (a World
            // row, a sky with no directional light), and churning a renderable
            // and a material clone for that would be absurd. Zero flags is the
            // BillboardSet2 rule applied to a Rectangle2D — visibility is an
            // any-bit test on the object's own flags.
            if (mSunDisc) mSunDisc->setVisibilityFlags(0u);
            return;
        }
        if (!mSunDisc) {
            mSunDisc = mSceneMgr->createRectangle2D(Ogre::SCENE_STATIC);
            // A screen-filling quad, no normals: our vertex program derives the
            // camera ray from the inverse view-projection instead of reading it
            // out of the normals, because nothing writes a second quad's
            // normals (JahSunDisc_vs's header).
            mSunDisc->initialize(Ogre::BT_DEFAULT, Ogre::Rectangle2D::GeometryFlagQuad);
            mSunDisc->setGeometry(-Ogre::Vector2::UNIT_SCALE, Ogre::Vector2(2.0f));
            // AND THEN update(), which is not optional: Rectangle2D's position
            // and size are UNINITIALISED members, initialize() fills the vertex
            // buffer from whatever they happen to hold, and setGeometry only
            // raises a dirty flag. Ogre's own sky survives this because
            // SceneManager::_renderPhase02 calls update() on it every frame;
            // nothing calls it on ours.
            mSunDisc->update();
            // The auto-params this quad's vertex shader needs (the real view
            // and projection) are handed to it as the IDENTITY while these two
            // flags — which Rectangle2D's constructor sets — are on.
            mSunDisc->setUseIdentityView(false);
            mSunDisc->setUseIdentityProjection(false);
            // QUEUE 5: after the sky (0) and long before opaque geometry (10),
            // so the scene occludes the disc through the depth test exactly as
            // it occludes the sky — and OUT of the sky capture's range, which
            // is queue 0 and the queue above it (JahshakaSkyCapture.compositor
            // has the measurement). It was 1, and at 1 a disc the user had put
            // in the probes was captured into the environment and became most
            // of the scene's ambient.
            mSunDisc->setRenderQueueGroup(5u);
            mSunDisc->setCastShadows(false);
            mSceneMgr->getRootSceneNode(Ogre::SCENE_STATIC)->attachObject(mSunDisc);
            mSceneMgr->notifyStaticAabbDirty(mSunDisc);
            // SCENE_STATIC: the static memory manager only recomputes what it
            // is TOLD is dirty, so an object attached after the first frame is
            // never visited and never drawn (CLAUDE.md's static-AABB rule).
            // Ogre's own sky gets away without this only because it is created
            // before anything has rendered.
            // A per-SCENE clone, for the same reason Ogre clones its sky
            // material per SceneManager: the parameters below are this scene's
            // sun, and two scenes in one process share the base material.
            Ogre::MaterialManager &mm = Ogre::MaterialManager::getSingleton();
            const Ogre::String name = "Jahshaka/SunDisc" +
                                      Ogre::StringConverter::toString(mSceneMgr->getId());
            mSunDiscMaterial = mm.getByName(name);
            if (!mSunDiscMaterial) {
                // AUTODETECT, not DEFAULT_RESOURCE_GROUP_NAME: our own media
                // folder is registered as its own group (the Hlms library path),
                // so a DEFAULT-group lookup finds nothing and the disc silently
                // never draws. This is the same load() every other material of
                // ours goes through (OgreChain's materialPass).
                Ogre::MaterialPtr base = std::static_pointer_cast<Ogre::Material>(
                    mm.load("Jahshaka/SunDisc",
                            Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME));
                if (!base) { mError = "sun disc: Jahshaka/SunDisc material is not staged"; return; }
                mSunDiscMaterial = base->clone(name);
                mSunDiscMaterial->load();
            }
            mSunDisc->setMaterial(mSunDiscMaterial);
        }
        if (!mSunDiscMaterial) return;

        // THE VISIBILITY CHANNEL (EnginePrivate.h, kSunDiscBit). kSunDiscBit
        // INSTEAD OF kVisibleBit keeps the disc out of every probe capture and
        // every shadow node for free; `inProbes` puts kVisibleBit back BESIDE
        // it, which is the owner's "both options" (pick 4).
        mSunDisc->setVisibilityFlags(kSunDiscBit | (sun.inProbes ? kVisibleBit : 0u));
        Ogre::Pass *pass = mSunDiscMaterial->getTechnique(0)->getPass(0);
        Ogre::GpuProgramParametersSharedPtr ps = pass->getFragmentProgramParameters();
        // w = cos(angular RADIUS); the document row is the DIAMETER, as every
        // renderer's "source angle" row is.
        const float radiusRad =
            float(std::max(0.0, double(sun.angularDiameterDeg)) * 0.5 * M_PI / 180.0);
        ps->setNamedConstant("sunDirection",
                             Ogre::Vector4(sun.dir[0], sun.dir[1], sun.dir[2],
                                           std::cos(radiusRad)));
        ps->setNamedConstant("sunColour",
                             Ogre::Vector4(sun.colour.r, sun.colour.g, sun.colour.b, 1.0f));
    } JAH_CATCH(mError, );
    // ...and, while a cloud layer is drawn, the disc seen THROUGH it wears the
    // clouded variant of this material with the same two numbers.
    syncSunDiscClouds();
}

void OgreScene::destroySunDisc() {
    if (mSunDisc) { mSceneMgr->destroyRectangle2D(mSunDisc); mSunDisc = nullptr; }
    mSunDiscMaterial.reset();
    // ...AND FORGET WHAT WAS PUSHED. destroySky() takes the disc with it (a
    // NoSky description is a full clear), so the quad is gone while mSkyDesc
    // still says a disc is enabled — and the next push, being value-equal,
    // would do nothing and leave the sun missing until something else moved it.
    mSkyDesc.sun = SunDisc();
}

void OgreScene::destroySky() {
    destroySunDisc();
    destroyCloudLayer();
    // The analytic sky goes with it — but the COMPONENT does not: the fog may
    // still want it (syncAtmosphere owns that decision).
    if (mAtmoSkyOn) { mAtmoSkyOn = false; syncAtmosphere(); }
    mSkyCapturePending = false;
    forgetSkySh();
    destroySkyShTicket();      // it was answering for a sky that is gone
    // Unbind the reflection cubemap from every datablock before it goes away.
    destroyReflection();
    if (mSceneMgr->getSky())
        mSceneMgr->setSky(false, mSceneMgr->getSkyMethod(), static_cast<Ogre::TextureGpu *>(nullptr));
    if (mSkyOwnedTex) {
        destroyRecycled(mRoot->getRenderSystem()->getTextureGpuManager(), mSkyOwnedTex);
        mSkyOwnedTex = nullptr;
    }
    if (mIblCamera) { mSceneMgr->destroyCamera(mIblCamera); mIblCamera = nullptr; }
}


// ---------------------------------------------------------------------------
// THE CLOUD LAYER (CLOUDS-2D-1; SPECS/CLOUDS_ASSESSMENT.md option C0)
// ---------------------------------------------------------------------------
// HDRP's Cloud Layer, in this engine's terms: ONE sheet of cloud at an altitude
// over a curved earth, drawn by a THIRD screen quad beside the sky's and the sun
// disc's (the disc's shape: our own low-level material, the camera ray derived
// from the inverse view-projection). Four pieces, each where it belongs:
//
//   * THE FIELD — the sheet's vertical optical depth over one 16 km tile, baked
//     into a 1024^2 R16F target from a fixed-seed Perlin-Worley noise (generated
//     once per process on the CPU, uploaded as a ManualTexture) times the
//     optional weather map. Re-baked on a change of coverage, density or map,
//     never per frame; the wind SCROLLS it (a uniform), it does not re-bake it.
//   * THE LAYER — render queue 0, subgroup 2: after the sky (subgroup 1), inside
//     the environment capture's range (queue 0, visibility 0x1), so the ambient
//     SH, the reflection cube and every Photon estimator that reads the one
//     environment see the sheet with no further work (PHOTON B3). Premultiplied
//     over the sky; lit by the sun (two-stream transmission plus a two-lobe
//     single scatter, self-shadowed through the field towards the sun) and by
//     the sky's own mean radiance (JahCloudLayer_ps.glsl says why each).
//   * THE DISC — the sun disc is drawn at queue 5, after the layer (it must stay
//     out of the capture), so while a layer exists it wears a clouded variant
//     of its material: the same disc times the sheet's transmittance along the
//     same ray (JahCloudLayer.glsl is the one copy of that geometry).
//   * THE GROUND SHADOW — the same field handed to HlmsPbs as a third extra
//     pass texture (FogHlmsListener, `jah_cloud_shadow`), multiplying the first
//     directional light's shadow factor by exp(-tau / mu_s) where the sun ray
//     from the pixel crosses the sheet.
//
// A DISABLED LAYER CREATES NOTHING: no quad, no texture, no pass property. A
// scene that never enables one generates every shader and draws every pixel
// exactly as it did before this lane (the 0048 pattern), which is what keeps
// the selftest hashes and every fixture where they are.
//
// THE CAPTURE CADENCE. A parameter change re-captures at once (setSky). While
// the sheet SCROLLS the environment it is captured into goes stale slowly — a
// cloud moves a few metres a frame against a 16 km tile — so the scroll
// re-captures every kCloudCaptureFrames drawn frames, with the asynchronous SH
// read (no GPU wait). A capture is not free downstream: a new SH re-captures the
// probes that read it, re-integrates the irradiance field and relights the
// surface cache's indirect half (the environment is in its signature). The
// period below is set from that measured cost.
namespace {
constexpr float    kCloudTileMetres   = 16000.0f;   // one tile of the field, in world metres
constexpr Ogre::uint32 kCloudFieldSize = 1024u;     // ~16 m a texel: shadows soft, cells resolved
constexpr Ogre::uint32 kCloudNoiseSize = 256u;
constexpr float    kCloudTauFull      = 32.0f;      // a full column's optical depth at density 1 (a thick stratocumulus deck; its base transmits ~22 % diffusely)
constexpr float    kCloudSlabMetres   = 1000.0f;    // the sheet's thickness the self-shadow crosses
constexpr float    kCloudFadeMetres   = 60000.0f;   // the distance the far sheet fades over
// THE SCROLL'S RE-CAPTURE PERIOD, SET FROM ITS MEASURED DOWNSTREAM COST
// (spikes/clouds-2d-1/cadence/, 2026-09-23: Debug, Xvfb, one process, three
// interleaved still/scroll pairs of 240 frames at a forced period of 10, the
// default tier — probe grid + irradiance field, cards off at every shipped
// tier). ONE scroll capture costs 26.4 ms of GPU and 45.7 ms of UI-thread CPU
// in total — 28.8 and 16.7 still frames — almost none of it the capture: a new
// SH stales the probe grid ("ambient", its whole grid re-captured at the
// budget's one probe a frame) and re-sweeps the irradiance field. At 90 frames
// that is +32 % GPU / +18 % CPU amortised; at 600 it is +4.8 % / +2.8 %. A
// sheet at 10 m/s moves 100 m in those 10 s against a field of kilometre
// cells, so the ambient it photographs has not moved; a parameter change
// still re-captures at once.
constexpr unsigned kCloudCaptureFrames = 600u;
/// The period in force: kCloudCaptureFrames, unless the run-wide measurement
/// latch JAHSHAKA_CLOUD_CAPTURE_FRAMES names another (read once — the A/B of
/// the cadence is a run of the shipped binary, like JAHSHAKA_SKY_SH_SYNC).
unsigned cloudCapturePeriod() {
    static const unsigned period = [] {
        const char *v = std::getenv("JAHSHAKA_CLOUD_CAPTURE_FRAMES");
        const long n = v ? std::strtol(v, nullptr, 10) : 0;
        return n > 0 ? unsigned(n) : kCloudCaptureFrames;
    }();
    return period;
}
const char *kCloudBakeWorkspace = "JahshakaCloudBakeWorkspace";

// ---- the noise: a fixed seed, integer hashing, every channel tiling --------
inline Ogre::uint32 cloudHash(Ogre::uint32 x, Ogre::uint32 y, Ogre::uint32 seed) {
    Ogre::uint32 h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}
inline float cloudRand01(Ogre::uint32 h) { return float(h & 0xFFFFFFu) / float(0x1000000u); }
inline Ogre::uint32 wrapCell(int i, int period) { return Ogre::uint32(((i % period) + period) % period); }

/// Gradient noise tiling at `period` lattice cells over [0,1).
float cloudPerlin(float u, float v, int period, Ogre::uint32 seed) {
    const float x = u * float(period), y = v * float(period);
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = x - float(ix), fy = y - float(iy);
    auto grad = [&](int cx, int cy, float dx, float dy) {
        const Ogre::uint32 h = cloudHash(wrapCell(cx, period), wrapCell(cy, period), seed);
        const float a = cloudRand01(h) * 6.28318531f;
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    const float n00 = grad(ix, iy, fx, fy), n10 = grad(ix + 1, iy, fx - 1.0f, fy);
    const float n01 = grad(ix, iy + 1, fx, fy - 1.0f), n11 = grad(ix + 1, iy + 1, fx - 1.0f, fy - 1.0f);
    const float sx = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float sy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    const float a = n00 + (n10 - n00) * sx, b = n01 + (n11 - n01) * sx;
    return a + (b - a) * sy;   // about [-0.7, 0.7]
}
float cloudPerlinFbm(float u, float v, int period, int octaves, Ogre::uint32 seed) {
    float sum = 0.0f, amp = 0.5f;
    for (int o = 0; o < octaves; ++o, period *= 2, amp *= 0.5f)
        sum += amp * cloudPerlin(u, v, period, seed + Ogre::uint32(o) * 101u);
    return sum;
}
/// 1 - the distance to the nearest feature point (Worley F1), tiling.
float cloudWorley(float u, float v, int period, Ogre::uint32 seed) {
    const float x = u * float(period), y = v * float(period);
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    float best = 4.0f;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = ix + dx, cy = iy + dy;
            const Ogre::uint32 h = cloudHash(wrapCell(cx, period), wrapCell(cy, period), seed);
            const float px = float(cx) + cloudRand01(h);
            const float py = float(cy) + cloudRand01(cloudHash(h, 0x9e3779b9u, seed));
            const float d2 = (px - x) * (px - x) + (py - y) * (py - y);
            best = std::min(best, d2);
        }
    return 1.0f - std::min(1.0f, std::sqrt(best));
}
float cloudWorleyFbm(float u, float v, int period, Ogre::uint32 seed) {
    return 0.625f * cloudWorley(u, v, period, seed) +
           0.25f  * cloudWorley(u, v, period * 2, seed + 7u) +
           0.125f * cloudWorley(u, v, period * 4, seed + 13u);
}

/// THE NOISE, ONCE PER PROCESS: 256^2 RGBA8 plus its box-filtered mip chain
/// (the bake samples it minified). R = Perlin-Worley cumulus, G = a finer
/// Worley (the edge erosion), B = low-frequency Perlin (large-scale
/// patchiness), A = high-frequency Perlin (reserved for wisps). Each channel is
/// stretched to its own measured [min, max] — deterministic, because the data
/// is. The seed is a constant: every run, every machine, the same sky.
const std::vector<std::vector<Ogre::uint8>> &cloudNoiseMips() {
    static const std::vector<std::vector<Ogre::uint8>> mips = [] {
        const Ogre::uint32 n = kCloudNoiseSize;
        const Ogre::uint32 kSeed = 0x6a09e667u;
        std::vector<float> ch[4];
        for (auto &c : ch) c.resize(size_t(n) * n);
        for (Ogre::uint32 y = 0; y < n; ++y)
            for (Ogre::uint32 x = 0; x < n; ++x) {
                const float u = (float(x) + 0.5f) / float(n), v = (float(y) + 0.5f) / float(n);
                const size_t i = size_t(y) * n + x;
                // Schneider's Perlin-Worley: the Perlin fbm remapped from the
                // inverted Worley fbm's floor — billowy cells with soft tops.
                const float perlin = 0.5f + cloudPerlinFbm(u, v, 4, 4, kSeed);
                const float worley = cloudWorleyFbm(u, v, 4, kSeed + 1u);
                const float lo = worley - 1.0f;
                ch[0][i] = (perlin - lo) / std::max(1.0f - lo, 1e-4f);
                ch[1][i] = cloudWorleyFbm(u, v, 8, kSeed + 2u);
                ch[2][i] = cloudPerlinFbm(u, v, 2, 3, kSeed + 3u);
                ch[3][i] = cloudPerlinFbm(u, v, 16, 3, kSeed + 4u);
            }
        std::vector<Ogre::uint8> base(size_t(n) * n * 4u);
        for (int c = 0; c < 4; ++c) {
            float lo = ch[c][0], hi = ch[c][0];
            for (float f : ch[c]) { lo = std::min(lo, f); hi = std::max(hi, f); }
            const float k = hi > lo ? 1.0f / (hi - lo) : 0.0f;
            for (size_t i = 0; i < ch[c].size(); ++i)
                base[i * 4u + size_t(c)] =
                    Ogre::uint8(std::lround(std::min(1.0f, std::max(0.0f, (ch[c][i] - lo) * k)) * 255.0f));
        }
        std::vector<std::vector<Ogre::uint8>> out;
        out.push_back(std::move(base));
        for (Ogre::uint32 m = n; m > 1u; m >>= 1) {
            const std::vector<Ogre::uint8> &src = out.back();
            const Ogre::uint32 h = m >> 1;
            std::vector<Ogre::uint8> dst(size_t(h) * h * 4u);
            for (Ogre::uint32 y = 0; y < h; ++y)
                for (Ogre::uint32 x = 0; x < h; ++x)
                    for (int c = 0; c < 4; ++c) {
                        const auto at = [&](Ogre::uint32 sx, Ogre::uint32 sy) {
                            return unsigned(src[(size_t(sy) * m + sx) * 4u + size_t(c)]);
                        };
                        dst[(size_t(y) * h + x) * 4u + size_t(c)] = Ogre::uint8(
                            (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) +
                             at(2 * x + 1, 2 * y + 1) + 2u) / 4u);
                    }
            out.push_back(std::move(dst));
        }
        return out;
    }();
    return mips;
}

/// A per-scene clone of one of our materials (the sun disc's reason: its
/// parameters are this scene's). Null when the media is not staged.
Ogre::MaterialPtr cloneForScene(const char *base, Ogre::SceneManager *sm) {
    Ogre::MaterialManager &mm = Ogre::MaterialManager::getSingleton();
    const Ogre::String name = Ogre::String(base) + Ogre::StringConverter::toString(sm->getId());
    Ogre::MaterialPtr m = mm.getByName(name);
    if (m) return m;
    Ogre::MaterialPtr src = std::static_pointer_cast<Ogre::Material>(
        mm.load(base, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME));
    if (!src) return Ogre::MaterialPtr();
    m = src->clone(name);
    m->load();
    // A material whose program failed to compile has no supported technique,
    // and drawing it takes the frame down in the pipeline build — refuse it
    // here, where the failure is one line in the log instead.
    if (m->getNumSupportedTechniques() == 0) return Ogre::MaterialPtr();
    return m;
}
}   // namespace

bool OgreScene::cloudLayerDrawn() const {
    return mSkyDesc.clouds.enabled && mSkyDesc.mode != SkyMode::NoSky && mCloudQuad &&
           mCloudMaterial && mCloudField;
}

void OgreScene::applyCloudLayer(bool fieldChanged) {
    const CloudLayerDesc &c = mSkyDesc.clouds;
    const unsigned period =
        (c.enabled && (c.wind[0] != 0.0f || c.wind[1] != 0.0f)) ? cloudCapturePeriod() : 0u;
    // A wind that STARTS starts its period: the first scroll capture is one
    // period after the sheet began to move, never whatever an earlier wind left.
    if (period && !mCloudStatus.capturePeriodFrames) mCloudFramesSinceCapture = 0u;
    mCloudStatus.capturePeriodFrames = period;
    if (!c.enabled || mSkyDesc.mode == SkyMode::NoSky) {
        // HIDDEN, NOT DESTROYED (the disc's rule): the layer is switched on and
        // off from a World row, and its field is 2 MB that a switch back on
        // would otherwise re-bake. The ground shadow goes with it at once.
        if (mCloudQuad) mCloudQuad->setVisibilityFlags(0u);
        FogHlmsListener::setCloudShadow(mSceneMgr, FogHlmsListener::CloudShadowState());
        mCloudStatus.drawn = false;
        mCloudStatus.reason = !c.enabled ? "off" : "noSky";
        syncSunDiscClouds();
        snapshotCloudGi();
        return;
    }
    JAH_TRY {
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
        if (!mCloudMaterial) mCloudMaterial = cloneForScene("Jahshaka/CloudLayer", mSceneMgr);
        // The bake's material is the BASE one, not a clone: a render_quad pass
        // names its material in the compositor script, and the bake binds its
        // inputs immediately before the one render that reads them.
        if (!mCloudBakeMaterial)
            mCloudBakeMaterial = std::static_pointer_cast<Ogre::Material>(
                Ogre::MaterialManager::getSingleton().load(
                    "Jahshaka/CloudBake", Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME));
        if (mCloudBakeMaterial && mCloudBakeMaterial->getNumSupportedTechniques() == 0)
            mCloudBakeMaterial.reset();
        if (!mCloudMaterial || !mCloudBakeMaterial) {
            mCloudStatus.drawn = false;
            mCloudStatus.reason = "media";
            mError = "clouds: Jahshaka/CloudLayer or Jahshaka/CloudBake is not staged";
            return;
        }
        // THE NOISE, uploaded once per scene from the process-wide copy.
        if (!mCloudNoise) {
            const auto &mips = cloudNoiseMips();
            mCloudNoise = tm->createTexture(recycledName("cloudnoise"),
                                            Ogre::GpuPageOutStrategy::SaveToSystemRam,
                                            Ogre::TextureFlags::ManualTexture,
                                            Ogre::TextureTypes::Type2D);
            mCloudNoise->setResolution(kCloudNoiseSize, kCloudNoiseSize);
            mCloudNoise->setPixelFormat(Ogre::PFG_RGBA8_UNORM);
            mCloudNoise->setNumMipmaps(Ogre::uint8(mips.size()));
            // Immediate, and NO notifyDataIsReady (DOCS/traps/ENGINE.md: a
            // ManualTexture's _transitionTo calls it itself).
            mCloudNoise->_transitionTo(Ogre::GpuResidency::Resident, (Ogre::uint8 *)0);
            mCloudNoise->_setNextResidencyStatus(Ogre::GpuResidency::Resident);
            for (size_t m = 0; m < mips.size(); ++m) {
                const Ogre::uint32 n = std::max(1u, kCloudNoiseSize >> m);
                Ogre::StagingTexture *staging =
                    tm->getStagingTexture(n, n, 1u, 1u, Ogre::PFG_RGBA8_UNORM);
                staging->startMapRegion();
                Ogre::TextureBox box = staging->mapRegion(n, n, 1u, 1u, Ogre::PFG_RGBA8_UNORM);
                for (Ogre::uint32 y = 0; y < n; ++y)
                    std::memcpy(box.at(0, y, 0), &mips[m][size_t(y) * n * 4u], size_t(n) * 4u);
                staging->stopMapRegion();
                staging->upload(box, mCloudNoise, Ogre::uint8(m), 0, 0);
                tm->removeStagingTexture(staging);
            }
        }
        // THE FIELD, a render target of our own with its mip chain.
        if (!mCloudField) {
            mCloudField = tm->createTexture(
                recycledName("cloudfield"), Ogre::GpuPageOutStrategy::Discard,
                Ogre::TextureFlags::RenderToTexture | Ogre::TextureFlags::AllowAutomipmaps,
                Ogre::TextureTypes::Type2D);
            mCloudField->setResolution(kCloudFieldSize, kCloudFieldSize);
            mCloudField->setPixelFormat(Ogre::PFG_R16_FLOAT);
            mCloudField->setNumMipmaps(
                Ogre::PixelFormatGpuUtils::getMaxMipmapCount(kCloudFieldSize, kCloudFieldSize));
            mCloudField->scheduleTransitionTo(Ogre::GpuResidency::Resident);
            fieldChanged = true;
        }
        if (fieldChanged) mCloudFieldPending = true;
        // THE QUAD, created once (applySunDisc's recipe and its three traps:
        // update() after setGeometry, the identity flags OFF, the static AABB).
        if (!mCloudQuad) {
            mCloudQuad = mSceneMgr->createRectangle2D(Ogre::SCENE_STATIC);
            mCloudQuad->initialize(Ogre::BT_DEFAULT, Ogre::Rectangle2D::GeometryFlagQuad);
            mCloudQuad->setGeometry(-Ogre::Vector2::UNIT_SCALE, Ogre::Vector2(2.0f));
            mCloudQuad->update();
            mCloudQuad->setUseIdentityView(false);
            mCloudQuad->setUseIdentityProjection(false);
            // QUEUE 0, SUBGROUP 2: after the sky quad (subgroup 1, which is
            // after a VR eye's hidden-area mesh at 0 — tuneSkyRenderable) and
            // inside the capture's range. The subgroup is the render queue's
            // top sort key, so the order is a guarantee and not a hash.
            mCloudQuad->setRenderQueueGroup(0u);
            mCloudQuad->setRenderQueueSubGroup(2u);
            mCloudQuad->setCastShadows(false);
            mSceneMgr->getRootSceneNode(Ogre::SCENE_STATIC)->attachObject(mCloudQuad);
            mSceneMgr->notifyStaticAabbDirty(mCloudQuad);
            mCloudQuad->setMaterial(mCloudMaterial);
        }
        // kVisibleBit ONLY: in every view and in the environment capture
        // (visibility 0x1), never GI geometry, never a shadow caster.
        mCloudQuad->setVisibilityFlags(kVisibleBit);
        Ogre::Pass *pass = mCloudMaterial->getTechnique(0)->getPass(0);
        if (Ogre::TextureUnitState *tu = pass->getTextureUnitState("cloudField"))
            tu->setTexture(mCloudField);
        Ogre::GpuProgramParametersSharedPtr ps = pass->getFragmentProgramParameters();
        const Ogre::Vector3 toSun =
            Ogre::Vector3(c.sunDir[0], c.sunDir[1], c.sunDir[2]).normalisedCopy();
        ps->setNamedConstant("cloudSun", Ogre::Vector4(toSun.x, toSun.y, toSun.z, c.hasSun ? 1.0f : 0.0f));
        ps->setNamedConstant("cloudSunE", Ogre::Vector4(c.sunIrradiance.r, c.sunIrradiance.g,
                                                        c.sunIrradiance.b, kCloudSlabMetres));
        mCloudStatus.drawn = true;
        mCloudStatus.reason.clear();
        if (!mCloudClearValid) mCloudClearPending = true;
    } JAH_CATCH(mError, );
    updateCloudLayer();   // the scroll, the SH term and the ground shadow, now
    syncSunDiscClouds();
    snapshotCloudGi();    // the voxels' and the cards' copy (CLOUDS-2D-2)
}

void OgreScene::bakeCloudField() {
    mCloudFieldPending = false;
    if (!mCloudField || !mCloudBakeMaterial) return;
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    if (!cm->hasWorkspaceDefinition(Ogre::IdString(kCloudBakeWorkspace))) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: " + std::string(kCloudBakeWorkspace) +
            " not found — the cloud layer has no field");
        return;
    }
    Ogre::CompositorWorkspace *ws = nullptr;
    JAH_TRY {
        if (!mCloudBakeCamera) mCloudBakeCamera = mSceneMgr->createCamera(processUniqueName("cloudcam"), false);
        // THE BAKE'S INPUTS, bound into the base material immediately before
        // the one render that reads them (every scene's bake does the same, so
        // the shared material never carries another scene's state into it).
        const CloudLayerDesc &c = mSkyDesc.clouds;
        Ogre::Pass *bake = mCloudBakeMaterial->getTechnique(0)->getPass(0);
        Ogre::TextureGpu *weather = nullptr;
        if (c.weatherMap) {
            auto it = mTextures.find(c.weatherMap);
            if (it != mTextures.end()) weather = it->second.texture;
        }
        if (weather) waitForTextureResident(weather);
        if (Ogre::TextureUnitState *tu = bake->getTextureUnitState("cloudNoise")) tu->setTexture(mCloudNoise);
        // No map: the noise stands in for the unit (never read — bakeParams.z
        // is 0), because an unbound unit is not a thing a pass may have.
        if (Ogre::TextureUnitState *tu = bake->getTextureUnitState("cloudWeather"))
            tu->setTexture(weather ? weather : mCloudNoise);
        bake->getFragmentProgramParameters()->setNamedConstant(
            "bakeParams", Ogre::Vector4(std::max(0.0f, std::min(1.0f, c.coverage)),
                                        std::max(0.0f, c.density) * kCloudTauFull,
                                        weather ? 1.0f : 0.0f, 0.0f));
        Ogre::CompositorChannelVec externals;
        externals.push_back(mCloudField);
        ws = cm->addWorkspace(mSceneMgr, externals, mCloudBakeCamera,
                              Ogre::IdString(kCloudBakeWorkspace), false);
        ws->_beginUpdate(false);
        ws->_update();
        ws->_endUpdate(false);
        cm->removeWorkspace(ws);
        ws = nullptr;
        mCloudField->_autogenerateMipmaps();
        // Sampled by HlmsPbs through the listener's pass slot: it has to be in
        // the layout a sampler expects (handOverForSampling's header).
        handOverForSampling(mRoot, mCloudField);
        ++mCloudStatus.fieldBakes;
        // THE FIELD'S PIXELS CHANGED: the voxels and the cards read them.
        snapshotCloudGi(true);
        return;
    } JAH_CATCH(mError, );
    if (ws) { try { cm->removeWorkspace(ws); } catch (...) {} }
    Ogre::LogManager::getSingleton().logMessage("Jahshaka: cloud field bake failed: " + mError);
}

// THE LAYER'S CLOCK, ONCE PER DRAWN FRAME AND FROM NOWHERE ELSE (OgreEngine's
// renderOneFrame; the fix round's F1: this used to live inside the constant
// push, which setSky also runs on every look change — a dragged sun over the
// realistic sky ticked it three times a frame). The frame delta the host
// pushed for this frame (the document's SimulationClock through
// Engine::setFixedFrameDelta — zero on a paused scene, a fixed 1/60 grid under
// a script). Then the constants for the frame.
void OgreScene::tickCloudClock() {
    if (!cloudLayerDrawn()) return;
    const CloudLayerDesc &c = mSkyDesc.clouds;
    const double dt = double(Ogre::ControllerManager::getSingleton().getFrameDelay());
    const bool scrolling = (c.wind[0] != 0.0f || c.wind[1] != 0.0f);
    if (scrolling && dt > 0.0) {
        ++mCloudStatus.clockTicks;
        mCloudClock += dt;
        // The capture cadence, in drawn frames that actually moved the sheet.
        if (++mCloudFramesSinceCapture >= cloudCapturePeriod()) {
            mCloudFramesSinceCapture = 0u;
            mSkyCaptureAsyncOnce = true;
            requestSkyCapture();
            ++mCloudStatus.scrollCaptures;
            updateCloudLayer();
            snapshotCloudGi();   // the GI inputs follow the scroll at the capture's cadence
            return;
        }
    }
    updateCloudLayer();
}

// THE LAYER'S CONSTANTS for the current clock: pushes only, advances nothing —
// safe to run on any edit.
void OgreScene::updateCloudLayer() {
    if (!cloudLayerDrawn()) return;
    const CloudLayerDesc &c = mSkyDesc.clouds;
    // The scroll, wrapped to the tile in DOUBLE so a long session keeps its
    // float precision (the field tiles; only the offset within a tile matters).
    for (int i = 0; i < 2; ++i) {
        const double off = -double(c.wind[i]) * mCloudClock;
        mCloudScroll[i] = float(off - std::floor(off / kCloudTileMetres) * kCloudTileMetres);
        mCloudStatus.scroll[i] = mCloudScroll[i];
    }
    JAH_TRY {
        const Ogre::Vector4 layer(c.altitude, 1.0f / kCloudTileMetres, mCloudScroll[0], mCloudScroll[1]);
        Ogre::Pass *pass = mCloudMaterial->getTechnique(0)->getPass(0);
        Ogre::GpuProgramParametersSharedPtr ps = pass->getFragmentProgramParameters();
        ps->setNamedConstant("cloudLayer", layer);
        pushCloudAmbient();
        if (mSunDiscCloudMaterial) {
            Ogre::GpuProgramParametersSharedPtr dp =
                mSunDiscCloudMaterial->getTechnique(0)->getPass(0)->getFragmentProgramParameters();
            dp->setNamedConstant("cloudLayer", layer);
        }
        // THE GROUND SHADOW's per-frame state (read by the listener per pass).
        FogHlmsListener::CloudShadowState st;
        const Ogre::Vector3 toSun =
            Ogre::Vector3(c.sunDir[0], c.sunDir[1], c.sunDir[2]).normalisedCopy();
        if (c.hasSun && toSun.y > 0.0f && c.shadow > 0.0f) {
            const float muS = std::max(toSun.y, 0.02f);
            st.field = mCloudField;
            st.invTile = 1.0f / kCloudTileMetres;
            st.strength = std::min(1.0f, c.shadow);
            st.scroll[0] = mCloudScroll[0];
            st.scroll[1] = mCloudScroll[1];
            st.altitude = c.altitude;
            st.sunThrow[0] = toSun.x / muS;
            st.sunThrow[1] = toSun.z / muS;
            st.invMuSun = 1.0f / muS;
        }
        FogHlmsListener::setCloudShadow(mSceneMgr, st);
    } JAH_CATCH(mError, );
}

// THE SKY LIGHT ON THE SHEET: the CLEAR sky's mean radiance (SH band 0 in this
// basis IS the mean over the sphere — ShAccum's k0), from a capture with the
// sheet hidden (captureCloudClearSky's header says why never the environment
// capture the sheet is itself in).
void OgreScene::pushCloudAmbient() {
    if (!mCloudMaterial) return;
    JAH_TRY {
        const Ogre::Vector4 amb = mCloudClearValid
            ? Ogre::Vector4(std::max(0.0f, mCloudClearMean[0]), std::max(0.0f, mCloudClearMean[1]),
                            std::max(0.0f, mCloudClearMean[2]), kCloudFadeMetres)
            : Ogre::Vector4(0.0f, 0.0f, 0.0f, kCloudFadeMetres);
        mCloudMaterial->getTechnique(0)->getPass(0)->getFragmentProgramParameters()
            ->setNamedConstant("cloudAmbient", amb);
    } JAH_CATCH(mError, );
}

void OgreScene::captureCloudClearSky() {
    mCloudClearPending = false;
    if (!mCloudQuad) return;
    // A GESTURE (a sun being dragged over an analytic sky re-captures every
    // frame) takes the ticket; a lone change reads synchronously, so the
    // capture right after this one in the same frame photographs a sheet lit
    // by the sky it is under. The same rule, and the same measurement, as the
    // environment's own SH (integrateSkyShFromCube's header).
    const bool consecutive = mSkyCaptureIdleFrames <= kSkyCaptureDragFrames;
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    Ogre::TextureGpu *cube = nullptr;
    Ogre::AsyncTextureTicket *ticket = nullptr;
    JAH_TRY {
        mCloudQuad->setVisibilityFlags(0u);
        try {
            cube = renderSkyCaptureCube("skyclear", kSkyCaptureSize, true);
        } catch (...) {
            mCloudQuad->setVisibilityFlags(kVisibleBit);
            throw;
        }
        mCloudQuad->setVisibilityFlags(kVisibleBit);
        cube->_autogenerateMipmaps();
        const Ogre::uint8 mip = std::min<Ogre::uint8>(kSkyShMip, Ogre::uint8(cube->getNumMipmaps() - 1u));
        const Ogre::uint32 n = std::max(1u, kSkyCaptureSize >> mip);
        readCloudClearTicket(true);   // never overwrite one: it owns a staging buffer
        ticket = tm->createAsyncTextureTicket(n, n, 6u, Ogre::TextureTypes::TypeCube,
                                              cube->getPixelFormat());
        if (mCloudClearValid && consecutive) {
            ticket->download(cube, mip, false);   // read at the next frame's top
            mCloudClearTicket = ticket;
            ticket = nullptr;
        } else {
            mRoot->getRenderSystem()->flushCommands();
            ticket->download(cube, mip, true);
            mCloudClearTicket = ticket;
            ticket = nullptr;
            readCloudClearTicket(true);
        }
        destroyRecycled(tm, cube);
        cube = nullptr;
    } JAH_CATCH(mError, );
    if (ticket) { try { tm->destroyAsyncTextureTicket(ticket); } catch (...) {} }
    if (cube) { try { destroyRecycled(tm, cube); } catch (...) {} }
}

void OgreScene::readCloudClearTicket(bool force) {
    if (!mCloudClearTicket) return;
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    JAH_TRY {
        if (!force && !mCloudClearTicket->queryIsTransferDone()) return;   // next frame
        const Ogre::TextureBox box = mCloudClearTicket->map(0);
        float sh[27];
        integrateSkyShFromBox(box, sh);
        mCloudClearTicket->unmap();
        for (int c = 0; c < 3; ++c) mCloudClearMean[c] = sh[c];
        mCloudClearValid = true;
        pushCloudAmbient();
    } JAH_CATCH(mError, );
    JAH_TRY { tm->destroyAsyncTextureTicket(mCloudClearTicket); } JAH_CATCH(mError, );
    mCloudClearTicket = nullptr;
}

void OgreScene::syncSunDiscClouds() {
    if (!mSunDisc || !mSunDiscMaterial) return;
    JAH_TRY {
        if (!cloudLayerDrawn()) {
            // THE DISC'S OWN MATERIAL, and exactly it: a scene with no layer
            // draws the disc it drew before the layer existed.
            if (mSunDisc->getMaterial() != mSunDiscMaterial) mSunDisc->setMaterial(mSunDiscMaterial);
            return;
        }
        if (!mSunDiscCloudMaterial) mSunDiscCloudMaterial = cloneForScene("Jahshaka/SunDiscClouded", mSceneMgr);
        if (!mSunDiscCloudMaterial) return;
        Ogre::Pass *dst = mSunDiscCloudMaterial->getTechnique(0)->getPass(0);
        Ogre::Pass *src = mSunDiscMaterial->getTechnique(0)->getPass(0);
        // The disc's two numbers, as applySunDisc wrote them.
        dst->getFragmentProgramParameters()->copyMatchingNamedConstantsFrom(
            *src->getFragmentProgramParameters());
        if (Ogre::TextureUnitState *tu = dst->getTextureUnitState("cloudField"))
            tu->setTexture(mCloudField);
        dst->getFragmentProgramParameters()->setNamedConstant(
            "cloudLayer", Ogre::Vector4(mSkyDesc.clouds.altitude, 1.0f / kCloudTileMetres,
                                        mCloudScroll[0], mCloudScroll[1]));
        if (mSunDisc->getMaterial() != mSunDiscCloudMaterial) mSunDisc->setMaterial(mSunDiscCloudMaterial);
    } JAH_CATCH(mError, );
}

void OgreScene::destroyCloudLayer() {
    FogHlmsListener::setCloudShadow(mSceneMgr, FogHlmsListener::CloudShadowState());
    // The GI copy goes with it (no re-injection here: the scene is being torn
    // down or its sky replaced, and both re-inject on their own).
    mCloudGiState = FogHlmsListener::CloudShadowState();
    ++mCloudGiSerial;
    if (mCloudQuad) { mSceneMgr->destroyRectangle2D(mCloudQuad); mCloudQuad = nullptr; }
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    // The clones keep their units: a destroyed texture nulls itself out of every
    // unit that holds it (TextureUnitState listens), and every clone is re-bound
    // before it draws again (applyCloudLayer, syncSunDiscClouds, bakeCloudField).
    if (mCloudField) { destroyRecycled(tm, mCloudField); mCloudField = nullptr; }
    if (mCloudNoise) { destroyRecycled(tm, mCloudNoise); mCloudNoise = nullptr; }
    if (mCloudBakeCamera) { mSceneMgr->destroyCamera(mCloudBakeCamera); mCloudBakeCamera = nullptr; }
    mCloudMaterial.reset();
    mCloudBakeMaterial.reset();
    mSunDiscCloudMaterial.reset();
    mCloudFieldPending = false;
    if (mCloudClearTicket) {
        JAH_TRY { tm->destroyAsyncTextureTicket(mCloudClearTicket); } JAH_CATCH(mError, );
        mCloudClearTicket = nullptr;
    }
    mCloudClearValid = false;
    mCloudClearPending = false;
    mCloudStatus.drawn = false;
    mCloudStatus.reason = "off";
    // ...AND FORGET WHAT WAS PUSHED (destroySunDisc's reason).
    mSkyDesc.clouds = CloudLayerDesc();
}

CloudStatus OgreScene::cloudStatus() const {
    CloudStatus st = mCloudStatus;
    st.drawn = cloudLayerDrawn();
    if (st.drawn) st.reason.clear();
    else if (!mSkyDesc.clouds.enabled) st.reason = "off";
    else if (mSkyDesc.mode == SkyMode::NoSky) st.reason = "noSky";
    else if (st.reason.empty()) st.reason = "media";
    return st;
}

// THE SKY AS A PICTURE (CLOUDS-2D-1's export bake; Engine.h has the contract).
// The capture workspace renders EXACTLY the environment's picture — the bound
// sky and, over it, the cloud layer — into a cube of the asked size; the cube
// is read back synchronously (an export is not a frame) and resampled into the
// viewer's lat-long convention on the CPU.
bool OgreScene::renderSkyEquirect(unsigned width, unsigned height, unsigned faceSize,
                                  float exposure, std::vector<unsigned char> &rgba) {
    if (mSkyDesc.mode == SkyMode::NoSky || width == 0 || height == 0 || faceSize == 0) return false;
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    if (!cm->hasWorkspaceDefinition(Ogre::IdString(kSkyCaptureWorkspace))) return false;
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    Ogre::TextureGpu *cube = nullptr;
    Ogre::AsyncTextureTicket *ticket = nullptr;
    bool ok = false;
    JAH_TRY {
        if (mCloudFieldPending) bakeCloudField();
        // A hand-driven workspace outside renderOneFrame updates its own graph
        // (DOCS/traps/ENGINE.md: otherwise the quads cull out at the origin).
        mSceneMgr->updateSceneGraph();
        cube = renderSkyCaptureCube("skyexport", faceSize, false);
        mRoot->getRenderSystem()->flushCommands();
        ticket = tm->createAsyncTextureTicket(faceSize, faceSize, 6u, Ogre::TextureTypes::TypeCube,
                                              cube->getPixelFormat());
        ticket->download(cube, 0, true);
        const Ogre::TextureBox box = ticket->map(0);
        rgba.assign(size_t(width) * height * 4u, 0u);
        const auto encode = [exposure](float v) {
            v = std::max(0.0f, v * exposure);
            const float srgb = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            return (unsigned char)std::lround(std::min(1.0f, std::max(0.0f, srgb)) * 255.0f);
        };
        for (unsigned y = 0; y < height; ++y) {
            // gltfexporter's stitchCubemapToEquirect convention, exactly.
            const float phi = (float(y) + 0.5f) / float(height) * float(M_PI);
            const float dy = std::cos(phi), sp = std::sin(phi);
            for (unsigned x = 0; x < width; ++x) {
                const float theta = (1.0f - (float(x) + 0.5f) / float(width)) * 2.0f * float(M_PI);
                const float d[3] = { sp * std::cos(theta), dy, sp * std::sin(theta) };
                int face = 0;
                float best = -2.0f;
                for (int f = 0; f < 6; ++f) {
                    const float k = d[0] * kFaceFwd[f][0] + d[1] * kFaceFwd[f][1] + d[2] * kFaceFwd[f][2];
                    if (k > best) { best = k; face = f; }
                }
                const float *rt = kFaceRight[face], *up = kFaceUp[face];
                const float nx = (d[0] * rt[0] + d[1] * rt[1] + d[2] * rt[2]) / best;
                const float ny = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / best;
                const unsigned px = std::min(faceSize - 1u, unsigned(std::max(0.0f, (nx + 1.0f) * 0.5f * float(faceSize))));
                const unsigned py = std::min(faceSize - 1u, unsigned(std::max(0.0f, (1.0f - ny) * 0.5f * float(faceSize))));
                const Ogre::uint16 *t = reinterpret_cast<const Ogre::uint16 *>(box.at(px, py, size_t(face)));
                unsigned char *o = &rgba[(size_t(y) * width + x) * 4u];
                o[0] = encode(Ogre::Bitwise::halfToFloat(t[0]));
                o[1] = encode(Ogre::Bitwise::halfToFloat(t[1]));
                o[2] = encode(Ogre::Bitwise::halfToFloat(t[2]));
                o[3] = 255u;
            }
        }
        ticket->unmap();
        ok = true;
    } JAH_CATCH(mError, false);
    if (ticket) { try { tm->destroyAsyncTextureTicket(ticket); } catch (...) {} }
    if (cube) { try { destroyRecycled(tm, cube); } catch (...) {} }
    return ok;
}


// ---------------------------------------------------------------------------
// THE CLOUD SHADOW ON THE VOXELS AND THE CARDS (CLOUDS-2D-2)
// ---------------------------------------------------------------------------
// The pixel's direct sun is darkened by the sheet (JahFog); so must every other
// estimate of the SAME direct term be, or the bounce under an overcast is lit
// by a clear sun: the voxel light injection (fork media, LightInjection) and
// the surface cache's card relight (JahCardLight) read the same field through
// the same function (the piece JahCloudShadow), at their own world points.
void OgreScene::snapshotCloudGi(bool fieldRebaked) {
    FogHlmsListener::CloudShadowState st;
    // NOT BEFORE THE FIELD HOLDS ITS PIXELS: an injection runs at the writer
    // point, which can come before this frame's bake — a voxel read through an
    // unbaked field reads whatever the allocation held. The bake re-snapshots.
    if (cloudLayerDrawn() && !mCloudFieldPending) st = FogHlmsListener::cloudShadow(mSceneMgr);
    const FogHlmsListener::CloudShadowState &o = mCloudGiState;
    const bool same = st.field == o.field && st.invTile == o.invTile &&
                      st.strength == o.strength && st.scroll[0] == o.scroll[0] &&
                      st.scroll[1] == o.scroll[1] && st.altitude == o.altitude &&
                      st.sunThrow[0] == o.sunThrow[0] && st.sunThrow[1] == o.sunThrow[1] &&
                      st.invMuSun == o.invMuSun;
    if (same && !(fieldRebaked && st.field)) return;
    mCloudGiState = st;
    ++mCloudGiSerial;
    // The voxels hold the direct term, so a change re-injects them (the light
    // tick's path: one injection over the voxels already there, or an owed tick
    // under a chain); the field re-integrates from them.
    if (mVctLighting) refreshGiLighting(false);
}

void OgreScene::bindCloudInjection(Ogre::VctLighting *lighting) {
    Ogre::HlmsCompute *hc = mRoot->getHlmsManager()->getComputeHlms();
    Ogre::HlmsComputeJob *job = hc ? hc->findComputeJobNoThrow("VCT/LightInjection") : nullptr;
    if (!job) return;
    JAH_TRY {
        const FogHlmsListener::CloudShadowState &st = mCloudGiState;
        const Ogre::HlmsSamplerblock *wrap =
            st.field ? FogHlmsListener::acquireWrapSampler(mRoot->getHlmsManager()) : nullptr;
        const bool on = st.field && wrap && lighting && lighting->getVoxelizer();
        // CHANGE-GUARDED: setNumTexUnits invalidates the PSO hash whatever it is
        // told, and a scene without a layer must leave the job exactly as the
        // fork's JSON built it (three textures, the property 0).
        const Ogre::uint8 units = on ? 4u : 3u;
        if (job->getNumTexUnits() != units) job->setNumTexUnits(units);
        if (job->getProperty("jah_cloud_shadow") != (on ? 1 : 0))
            job->setProperty("jah_cloud_shadow", on ? 1 : 0);
        if (!on) return;
        Ogre::DescriptorSetTexture2::TextureSlot slot(
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
        slot.texture = st.field;
        job->setTexture(3u, slot, wrap);
        Ogre::ShaderParams &params = job->getShaderParams("default");
        const Ogre::Vector3 origin = lighting->getVoxelizer()->getVoxelOrigin();
        if (Ogre::ShaderParams::Param *p = params.findParameter("jahCloudMap"))
            p->setManualValue(Ogre::Vector4(st.invTile, st.strength, st.scroll[0], st.scroll[1]));
        if (Ogre::ShaderParams::Param *p = params.findParameter("jahCloudSun"))
            p->setManualValue(Ogre::Vector4(st.sunThrow[0], st.sunThrow[1], st.altitude, st.invMuSun));
        if (Ogre::ShaderParams::Param *p = params.findParameter("jahCloudOrigin"))
            p->setManualValue(Ogre::Vector4(origin.x, origin.y, origin.z, 0.0f));
        params.setDirty();
    } JAH_CATCH(mError, );
}

}}}  // namespace jahshaka::engine::detail
