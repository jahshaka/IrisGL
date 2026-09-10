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
#include "EnginePrivate.h"

#include <OgreMaterial.h>
#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreTextureUnitState.h>

namespace jahshaka { namespace engine { namespace detail {

namespace {
// Ogre's cubemap lookups are LEFT-handed (see buildCubeFromWorldFaces' comment).
// Destination slice d takes source WORLD face kSrcFace[d], mirrored as flagged.
const int  kSrcFace[6] = { 0, 1, 2, 3, 5, 4 };   // +Z and -Z swap
const bool kFlipH[6]   = { true, true, false, false, true, true };
const bool kFlipV[6]   = { false, false, true, true, false, false };

const char *kIblWorkspace = "JahshakaIblSpecularWorkspace";
}  // namespace

// THE ONE SKY ENTRY POINT (ENGINEERING_DEBT_SPEC.md item 4). It owns the three
// things the host used to: the dispatch on mode, the ordering (sky first, then
// reflections — destroySky takes the reflection cubemap with it, so the reverse
// order would throw away reflections that were just built), and the
// already-applied comparison, per HALF: a description that changes only its
// reflection faces must not tear the sky down and back up, because that is a
// texture upload plus a six-face cube build for nothing.
bool OgreScene::setSky(const SkyDesc &desc) {
    const bool skyChanged  = !mSkyDesc.sameSky(desc);
    const bool reflChanged = !mSkyDesc.sameReflections(desc);
    if (!skyChanged && !reflChanged) return true;   // idempotent: nothing to do
    bool ok = true;
    if (skyChanged) {
        if (applySkyMode(desc)) {
            mSkyDesc.mode = desc.mode;
            mSkyDesc.equirect = desc.equirect;
            for (int i = 0; i < 6; ++i) mSkyDesc.faces[i] = desc.faces[i];
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
        mSkyIsEquirect = true;
        mSceneMgr->setSky(true, Ogre::SceneManager::SkyEquirectangular, use);
        tuneSkyRenderable();
        // Only now is the old texture unreferenced by the sky material.
        if (previous && previous != owned)
            mRoot->getRenderSystem()->getTextureGpuManager()->destroyTexture(previous);
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
        mSkyIsEquirect = false;
        mSceneMgr->setSky(true, Ogre::SceneManager::SkyCubemap, cube);
        tuneSkyRenderable();
        if (previous)
            mRoot->getRenderSystem()->getTextureGpuManager()->destroyTexture(previous);
        return true;
    } JAH_CATCH(mError, false);
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
    if (mSkyIsEquirect && skyMat && skyMat->getNumTechniques() > 0 &&
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
    // Instant Radiosity casts rays with mVisibilityMask = kGiGeometryBit; the sky
    // must never be hit by them (nor counted as GI geometry anywhere else).
    sky->setVisibilityFlags(kVisibleBit);
}

Ogre::TextureGpu *OgreScene::makeSkyArrayTexture(Ogre::TextureGpu *src) {
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    Ogre::TextureGpu *dst = tm->createTexture(
        processUniqueName("skyarray"), Ogre::GpuPageOutStrategy::Discard,
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
        processUniqueName(namePrefix.c_str()), Ogre::GpuPageOutStrategy::Discard,
        Ogre::TextureFlags::ManualTexture | extraFlags, Ogre::TextureTypes::TypeCube);
    cube->setResolution(w, h, 6u);
    cube->setPixelFormat(pf);
    cube->setNumMipmaps(mips ? Ogre::PixelFormatGpuUtils::getMaxMipmapCount(w, h) : 1u);
    cube->_transitionTo(Ogre::GpuResidency::Resident, nullptr);

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
        }
        ticket->unmap();
        tm->destroyAsyncTextureTicket(ticket);
    }
    staging->stopMapRegion();
    staging->upload(dst, cube, 0, nullptr, nullptr, true);
    tm->removeStagingTexture(staging);
    return cube;
}

// EVERY sky change lands here, so this is where the environment cubemap is
// freed and immediately re-allocated — and the allocator hands the replacement
// back at the SAME ADDRESS routinely (observed on every re-bake of the Showroom
// scene: `destroyReflection refl=0x55556cb5e400` then `new cube=0x55556cb5e400`).
// That matters because Ogre identifies a texture by its TextureGpu POINTER in
// two caches that outlive it: VulkanTextureGpuManager::mCachedTex (the image
// views a descriptor set is built from) and DescriptorSetTexture::operator!=
// (which is how bakeTextures decides a datablock's set is unchanged and can be
// kept). A recycled address therefore makes an old, dead view look current.
// destroyReflection() below is what keeps that safe: it lets Ogre's
// TextureGpuListener::Deleted run so the old descriptor sets die WITH the old
// texture. Do not "optimise" the order there.
void OgreScene::buildReflectionCubemapFrom(Ogre::TextureGpu *srcCube, bool ownsSource) {
    destroyReflection();
    mIblSourceTex = srcCube;
    mIblSourceOwned = ownsSource;
    const Ogre::uint32 w = srcCube->getWidth(), h = srcCube->getHeight();
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    // The OUTPUT the PBR datablocks sample: same size, mipped, and a UAV, which
    // is what the compute integrator writes through.
    Ogre::TextureGpu *cube = tm->createTexture(
        processUniqueName("skyrefl"), Ogre::GpuPageOutStrategy::Discard,
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
    mReflectionTex = cube;
    // TELL HlmsPbs HOW MANY MIPS THE PROBE HAS. Without this the roughness->LOD
    // map (envSpecularRoughness, 800.PixelShader_piece_ps.any:4) multiplies by
    // passBuf.envMapNumMipmaps, which stays at its 1.0 default for a plain
    // PBSM_REFLECTION texture — only the PCC classes ever call this. Every
    // reflection was therefore sampled at mip 0-1 no matter how rough the
    // surface: a prefiltered chain nothing reads. (Pre-existing: the box mip
    // chain this replaces was equally unread.)
    static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS))
        ->_notifyIblSpecMipmap(cube->getNumMipmaps());
    // The convolution is a compute dispatch: queue it for the next frame, where
    // a command buffer exists (same contract as applyPendingGi).
    mIblPending = true;
    applyReflectionToAll();
}

void OgreScene::applyPendingIbl() {
    if (!mIblPending) return;
    mIblPending = false;
    if (!mIblSourceTex || !mReflectionTex) return;
    // A cube we own is scratch: once the convolution has read it, its (mipped,
    // full-size) VRAM is dead weight until the next sky change.
    struct FreeSource {
        OgreScene *self;
        ~FreeSource() {
            if (!self->mIblSourceOwned || !self->mIblSourceTex) return;
            try {
                self->mRoot->getRenderSystem()->getTextureGpuManager()->destroyTexture(
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
            mIblSourceTex->copyTo(mReflectionTex, mReflectionTex->getEmptyBox(0), 0,
                                  mIblSourceTex->getEmptyBox(0), 0);
            mReflectionTex->_autogenerateMipmaps();
            return;
        }
        if (!mIblCamera) mIblCamera = mSceneMgr->createCamera(processUniqueName("iblcam"), false);
        Ogre::CompositorChannelVec externals;
        externals.push_back(mIblSourceTex);
        externals.push_back(mReflectionTex);
        ws = cm->addWorkspace(mSceneMgr, externals, mIblCamera,
                              Ogre::IdString(kIblWorkspace), false);
        ws->_beginUpdate(false);
        ws->_update();
        ws->_endUpdate(false);
        cm->removeWorkspace(ws);
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
        mIblSourceTex->copyTo(mReflectionTex, mReflectionTex->getEmptyBox(0), 0,
                              mIblSourceTex->getEmptyBox(0), 0);
        mReflectionTex->_autogenerateMipmaps();
    } JAH_CATCH(mError, );
}

void OgreScene::destroyReflection() {
    mIblPending = false;
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    if (mIblSourceTex && mIblSourceOwned) tm->destroyTexture(mIblSourceTex);
    mIblSourceTex = nullptr;
    mIblSourceOwned = false;
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
    try { tm->destroyTexture(tex); }
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
// reflections P3/P6 lane; it is why `reflectionTexForDatablocks()` exists at all
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
// So while auto PCC is bound WE do not bind the IBL cubemap. Nothing is lost
// visually: the probe captures are full scene renders that include the sky, so
// the probes ARE the environment — sharper than the single global cubemap was,
// because they are parallax-corrected to the room. rebuildVct/teardownVct call
// applyReflectionToAll() so the binding follows the hybrid up and down.
//
// Before this, picking VCT+Probes on any scene with a sky produced a shader that
// did not compile — i.e. objects that did not draw at all — and it was invisible
// to every suite because no suite combined the two. `gi.pcc_mirror`'s sky case
// is the fence; it goes black without this.
Ogre::TextureGpu *OgreScene::reflectionTexForDatablocks() const {
    return mPcc ? nullptr : mReflectionTex;
}

void OgreScene::applyReflectionToAllImpl() {
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

void OgreScene::destroySky() {
    // Unbind the reflection cubemap from every datablock before it goes away.
    destroyReflection();
    if (mSceneMgr->getSky())
        mSceneMgr->setSky(false, mSceneMgr->getSkyMethod(), static_cast<Ogre::TextureGpu *>(nullptr));
    if (mSkyOwnedTex) {
        mRoot->getRenderSystem()->getTextureGpuManager()->destroyTexture(mSkyOwnedTex);
        mSkyOwnedTex = nullptr;
    }
    if (mIblCamera) { mSceneMgr->destroyCamera(mIblCamera); mIblCamera = nullptr; }
}

}}}  // namespace jahshaka::engine::detail
