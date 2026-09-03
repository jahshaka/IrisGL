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

bool OgreScene::setSky(SkyMode mode, TextureId texId) {
    JAH_TRY {
        if (mode == SkyMode::NoSky) { destroySky(); return true; }
        if (mode == SkyMode::Cubemap) {
            mError = "setSky: cubemap skies go through setSkyCubemap()";
            return false;
        }
        auto it = mTextures.find(texId);
        if (it == mTextures.end()) { mError = "setSky: unknown texture"; return false; }
        Ogre::TextureGpu *src = it->second.texture;
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

bool OgreScene::setSkyCubemap(const TextureId faces[6]) {
    JAH_TRY {
        Ogre::TextureGpu *tex[6];
        for (int i = 0; i < 6; ++i) {
            auto it = mTextures.find(faces[i]);
            if (it == mTextures.end()) { mError = "setSkyCubemap: unknown face texture"; return false; }
            tex[i] = it->second.texture;
        }
        for (int i = 0; i < 6; ++i)
            if (tex[i]->getWidth() != tex[0]->getWidth() || tex[i]->getHeight() != tex[0]->getHeight()) {
                mError = "setSkyCubemap: the six faces must all be the same size";
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

bool OgreScene::setSkyReflection(const TextureId faces[6]) {
    JAH_TRY {
        bool anySet = false;
        for (int i = 0; i < 6; ++i) if (faces[i]) anySet = true;
        if (!anySet) { destroyReflection(); return true; }
        Ogre::TextureGpu *tex[6];
        for (int i = 0; i < 6; ++i) {
            auto it = mTextures.find(faces[i]);
            if (it == mTextures.end()) { mError = "setSkyReflection: unknown face texture"; return false; }
            tex[i] = it->second.texture;
        }
        for (int i = 0; i < 6; ++i)
            if (tex[i]->getWidth() != tex[0]->getWidth() || tex[i]->getHeight() != tex[0]->getWidth()) {
                mError = "setSkyReflection: the six faces must be square and the same size";
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
    // is AFTER our on-top overlays at 200 — a gizmo drawn over empty sky would be
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
    applyReflectionToAll();
    tm->destroyTexture(tex);
    // Recompute envMapNumMipmaps from whatever reflection textures remain
    // (_notifyIblSpecMipmap only ever grows it).
    static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS))
        ->resetIblSpecMipmap(0u);
}

void OgreScene::applyReflectionToAll() { applyReflectionToAllImpl(); }

void OgreScene::applyReflectionToAllImpl() {
    auto *hlmsPbs = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS);
    for (auto &kv : mMaterials) {
        if (kv.second.unlit) continue;
        auto *db = static_cast<Ogre::HlmsPbsDatablock *>(hlmsPbs->getDatablock(Ogre::IdString(kv.second.datablockName)));
        if (db) db->setTexture(Ogre::PBSM_REFLECTION, mReflectionTex);
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
