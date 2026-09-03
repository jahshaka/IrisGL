// Materials (PBR, unlit, outline), textures and the mesh/material attachment
// verbs that bind them onto a node.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

Ogre::uint8 OgreScene::renderQueueFor(const MaterialRec &m) {
    if (m.onTop)      return kOverlayRenderQueue;      // gizmos, wires, always-on-top
    if (m.refractive) return kRefractiveRenderQueue;   // the chain's refraction pass
    return 10u;                                        // Ogre's default for normal items
}

void OgreScene::refileItems(MaterialId id, const MaterialRec &m) {
    const Ogre::uint8 rq = renderQueueFor(m);
    for (auto &kv : mNodes)
        if (kv.second.materialRef == id && kv.second.item)
            kv.second.item->setRenderQueueGroup(rq);
}

Ogre::Hlms *OgreScene::hlmsFor(const MaterialRec &m) const {
    return mRoot->getHlmsManager()->getHlms(m.unlit ? Ogre::HLMS_UNLIT : Ogre::HLMS_PBS);
}

void OgreScene::setRefractionsActive(bool active) {
    if (active == mRefractionsActive) return;
    mRefractionsActive = active;
    JAH_TRY {
        auto *hlmsPbs = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS);
        for (auto &kv : mMaterials) {
            if (!kv.second.refractive || kv.second.unlit) continue;
            auto *db = static_cast<Ogre::HlmsPbsDatablock *>(
                hlmsPbs->getDatablock(Ogre::IdString(kv.second.datablockName)));
            if (!db) continue;
            db->setTransparency(db->getTransparency(),
                                active ? Ogre::HlmsPbsDatablock::Refractive
                                       : Ogre::HlmsPbsDatablock::Transparent);
        }
    } JAH_CATCH(mError, );
}

void OgreScene::applyPbr(Ogre::HlmsPbsDatablock *db, const PbrParams &p,
                         bool refractionsActive) {
    db->setDiffuse(Ogre::Vector3(p.albedo.r, p.albedo.g, p.albedo.b));
    db->setMetalness(p.metalness);
    db->setRoughness(p.roughness);
    db->setEmissive(Ogre::Vector3(p.emissive.r, p.emissive.g, p.emissive.b));
    db->setNormalMapWeight(p.normalMapWeight);
    // UV tiling: HlmsPbs has no UV transform for its base maps (only detail maps
    // have offset/scale), so the scale rides in the datablock's user values and a
    // custom_ps_uv_modifier_macros piece (JahFog_piece_vs_piece_ps.any, a library
    // folder of HlmsPbs — no longer attached per datablock) multiplies every
    // base-map lookup by material.userValue[0].xy. setUserValue only schedules a
    // const-buffer update — scale edits never recompile shaders.
    db->setUserValue(0, Ogre::Vector4(p.uvScale, p.uvScale, 1.0f, 1.0f));
    // Manage the macroblock ourselves: setTwoSidedLighting(changeMacroblock=true)
    // swaps culling to CULL_NONE when enabling but never restores it when
    // disabling, and applyPbr must be idempotent in both directions.
    db->setTwoSidedLighting(p.twoSided, false);
    {
        Ogre::HlmsMacroblock macro = *db->getMacroblock();
        const Ogre::CullingMode want = p.twoSided ? Ogre::CULL_NONE : Ogre::CULL_CLOCKWISE;
        if (macro.mCullMode != want) { macro.mCullMode = want; db->setMacroblock(macro); }
    }
    // NOTE HlmsPbs has NO ambient-occlusion slot and no roughness remap: the
    // document's occlusionMap/Factor stay unsupported (see Types.h), and
    // roughness bounds are clamped by the caller before they reach here.
    switch (p.alphaMode) {
    case PbrAlphaMode::Opaque:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        db->setTransparency(1.0f, kTransparencyNone);
        break;
    case PbrAlphaMode::Cutout:
        // The Hlms template discards when `threshold CMP alpha` is true
        // (threshold on the LEFT — 800.PixelShader_piece_ps.any:300), so
        // CMPF_GREATER discards alpha < cutoff: glTF MASK semantics. The
        // compared alpha comes from the diffuse texture; with no diffuse
        // texture it is 1.0.
        db->setTransparency(1.0f, kTransparencyNone);
        db->setAlphaTest(Ogre::CMPF_GREATER);
        db->setAlphaTestThreshold(p.alphaCutoff);
        break;
    case PbrAlphaMode::Blend:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        // Fade = plain alpha blending (glTF BLEND); imports keep spec semantics.
        db->setTransparency(p.alpha, Ogre::HlmsPbsDatablock::Fade);
        break;
    case PbrAlphaMode::Glass:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        // Ogre's own words: "realistic transparency that preserves lighting
        // reflections (particularly specular on the edges). Great for glass."
        // Fade here was why authored glass looked merely faded.
        db->setTransparency(p.alpha, Ogre::HlmsPbsDatablock::Transparent);
        break;
    case PbrAlphaMode::Additive:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        // Fade carries alpha into the fragment's output .a (it does NOT scale
        // the colour in-shader — the blend factor does that), and
        // changeBlendblock=false keeps SBT_TRANSPARENT_ALPHA out: the
        // blendblock below is SRC_ALPHA/ONE, so Final = Src·alpha + Dest —
        // alpha is the glow intensity, exactly three.js AdditiveBlending.
        db->setTransparency(p.alpha, Ogre::HlmsPbsDatablock::Fade, true, false);
        break;
    case PbrAlphaMode::Refractive:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        // Ogre: "similar to transparent, but also performs refractions. The
        // compositor scene pass must be set to render refractive objects in its
        // own pass" — that pass is the chain's RQ-200 pass (OgreChain.cpp).
        // Without it the generated shader references an undeclared refractionMap
        // and fails to compile, taking the whole frame with it — so outside a
        // refraction pass the material renders as ordinary glass instead
        // (setRefractionsActive owns that decision).
        db->setTransparency(p.alpha, refractionsActive
                                         ? Ogre::HlmsPbsDatablock::Refractive
                                         : Ogre::HlmsPbsDatablock::Transparent);
        db->setRefractionStrength(p.refractionStrength);
        break;
    case PbrAlphaMode::Modulate:
        db->setAlphaTest(Ogre::CMPF_ALWAYS_PASS);
        // No shader-side transparency: Final = Src × Dest via the blendblock
        // below (SBT_MODULATE). alpha deliberately ignored — scaling the colour
        // toward black would darken harder, not fade the effect out.
        db->setTransparency(1.0f, kTransparencyNone, true, false);
        break;
    }
    // Additive/Modulate ride an explicit blendblock preset (setTransparency
    // only knows alpha blending) and never write depth — like other transparents
    // they must not occlude what they blend over. Managed idempotently both
    // ways, the same discipline as the culling macroblock above.
    {
        const bool srcDest = p.alphaMode == PbrAlphaMode::Additive ||
                             p.alphaMode == PbrAlphaMode::Modulate;
        Ogre::HlmsBlendblock want = *db->getBlendblock();
        if (p.alphaMode == PbrAlphaMode::Additive) {
            // SRC_ALPHA/ONE, not SBT_ADD's ONE/ONE: Fade puts alpha in .a and
            // the source factor scales the contribution by it.
            want.mSeparateBlend = false;
            want.mSourceBlendFactor      = Ogre::SBF_SOURCE_ALPHA;
            want.mDestBlendFactor        = Ogre::SBF_ONE;
            want.mSourceBlendFactorAlpha = Ogre::SBF_SOURCE_ALPHA;
            want.mDestBlendFactorAlpha   = Ogre::SBF_ONE;
        }
        else if (p.alphaMode == PbrAlphaMode::Modulate)
            want.setBlendType(Ogre::SBT_MODULATE);
        else if (p.alphaMode == PbrAlphaMode::Opaque || p.alphaMode == PbrAlphaMode::Cutout)
            want.setBlendType(Ogre::SBT_REPLACE);   // undo a previous Additive/Modulate
        const Ogre::HlmsBlendblock &cur = *db->getBlendblock();
        if (want.mSourceBlendFactor != cur.mSourceBlendFactor ||
            want.mDestBlendFactor != cur.mDestBlendFactor ||
            want.mSourceBlendFactorAlpha != cur.mSourceBlendFactorAlpha ||
            want.mDestBlendFactorAlpha != cur.mDestBlendFactorAlpha ||
            want.mSeparateBlend != cur.mSeparateBlend)
            db->setBlendblock(want);
        Ogre::HlmsMacroblock macro = *db->getMacroblock();
        const bool wantDepthWrite = !srcDest;
        if (macro.mDepthWrite != wantDepthWrite) {
            macro.mDepthWrite = wantDepthWrite;
            db->setMacroblock(macro);
        }
    }
    // Alpha-to-coverage on Cutout ONLY, and only when the target is actually
    // multisampled (A2cEnabledMsaaOnly): MSAA then dithers the hard alpha-test
    // edge into coverage samples instead of a 1px staircase. Free at 1x; a PSO
    // rebuild for cutout datablocks only. AFTER the switch: setTransparency
    // rewrites the blendblock, and applyPbr must stay idempotent both ways.
    {
        const Ogre::uint8 want = static_cast<Ogre::uint8>(
            p.alphaMode == PbrAlphaMode::Cutout ? Ogre::HlmsBlendblock::A2cEnabledMsaaOnly
                                                : Ogre::HlmsBlendblock::A2cDisabled);
        if (db->getBlendblock()->mAlphaToCoverage != want) {
            Ogre::HlmsBlendblock bb = *db->getBlendblock();
            bb.mAlphaToCoverage = want;
            db->setBlendblock(bb);
        }
    }
}

// ---- Materials ----
MaterialId OgreScene::createPbrMaterial(const PbrParams &p) {
    JAH_TRY {
        MaterialRec rec; rec.datablockName = processUniqueName("pbr");
        rec.refractive = p.alphaMode == PbrAlphaMode::Refractive;
        auto *hlmsPbs = static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        auto *db = static_cast<Ogre::HlmsPbsDatablock *>(hlmsPbs->createDatablock(
            Ogre::IdString(rec.datablockName), rec.datablockName,
            Ogre::HlmsMacroblock(), Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        db->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);
        applyPbr(db, p, mRefractionsActive);
        if (mReflectionTex) db->setTexture(Ogre::PBSM_REFLECTION, mReflectionTex);
        mMaterials[++mNextMaterialId] = rec;
        return mNextMaterialId;
    } JAH_CATCH(mError, 0);
}

bool OgreScene::setPbrMaterial(MaterialId id, const PbrParams &p) {
    auto it = mMaterials.find(id);
    if (it == mMaterials.end()) return false;
    if (it->second.unlit) { mError = "setPbrMaterial: material is unlit"; return false; }
    JAH_TRY {
        auto *hlmsPbs = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS);
        auto *db = static_cast<Ogre::HlmsPbsDatablock *>(hlmsPbs->getDatablock(Ogre::IdString(it->second.datablockName)));
        if (!db) return false;
        applyPbr(db, p, mRefractionsActive);
        // An alpha-mode change moves the item between render queues, and the
        // items already exist: re-file them or a material turned refractive
        // keeps rendering in the opaque pass (as plain glass) until something
        // else happens to re-attach it.
        const bool wasRefractive = it->second.refractive;
        it->second.refractive = p.alphaMode == PbrAlphaMode::Refractive;
        if (wasRefractive != it->second.refractive) refileItems(id, it->second);
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::destroyMaterial(MaterialId id) {
    auto it = mMaterials.find(id);
    if (it == mMaterials.end()) return false;
    JAH_TRY {
        invalidateGiCaches();   // VctMaterial caches conversions by raw datablock pointer
        for (auto &kv : mNodes) if (kv.second.materialRef == id) detachItem(kv.first, kv.second);
        Ogre::Hlms *hlms = hlmsFor(it->second);
        if (hlms->getDatablock(Ogre::IdString(it->second.datablockName)))
            hlms->destroyDatablock(Ogre::IdString(it->second.datablockName));
        mMaterials.erase(it);
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::attachMesh(NodeId id, MeshId meshId, MaterialId matId) {
    auto nit = mNodes.find(id); auto mit = mMeshes.find(meshId); auto tit = mMaterials.find(matId);
    if (nit == mNodes.end()) { mError = "attachMesh: unknown node"; return false; }
    if (mit == mMeshes.end()) { mError = "attachMesh: unknown mesh"; return false; }
    if (tit == mMaterials.end()) { mError = "attachMesh: unknown material"; return false; }
    JAH_TRY {
        Node &n = nit->second;
        detachItem(id, n);
        n.item = mSceneMgr->createItem(mit->second.mesh, Ogre::SCENE_DYNAMIC);
        n.item->setDatablock(hlmsFor(tit->second)->getDatablock(Ogre::IdString(tit->second.datablockName)));
        // Only lit (PBR) surfaces participate in GI; unlit overlays, wires and
        // line meshes must neither bounce nor occlude the radiosity rays.
        n.item->setVisibilityFlags(tit->second.unlit ? kVisibleBit
                                                     : (kVisibleBit | kGiGeometryBit));
        // Render-queue policy (POST_CHAIN_SPEC.md §6): on-top overlays go in the
        // chain's overlay pass, refractive items in its refraction pass, and
        // everything else stays on Ogre's default queue.
        n.item->setRenderQueueGroup(renderQueueFor(tit->second));
        n.node->attachObject(n.item);
        n.meshRef = meshId; n.materialRef = matId;
        // New lit geometry must join the voxel volume / next trace; unlit
        // overlays (outlines, wires) never participate in GI.
        if (!tit->second.unlit) invalidateGiCaches();
        // A reflector node whose mesh was swapped keeps its flag (detachItem
        // above only disarmed the dead Item) — re-derive the plane from the new
        // geometry. A failure here is not fatal to attachMesh: the node simply
        // stops reflecting and lastError() says why.
        if (mReflectors.count(id)) armReflector(id, n);
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::detachMesh(NodeId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return false;
    JAH_TRY { detachItem(id, it->second); return true; } JAH_CATCH(mError, false);
}

// ---- Textures ----
TextureId OgreScene::loadTexture(const std::string &path, bool srgb) {
    // Path dedup, but NEVER across the decal atlases: the same image file can be
    // both an ordinary PBR map and a decal image, and they live in different
    // pools with different formats. Handing a decal slice back from here would
    // bind a pooled slice as a base map AND let the caller destroyTexture() a
    // texture other scenes' decals are still sampling.
    for (auto &kv : mTextures)
        if (!kv.second.decal && kv.second.path == path) return kv.first;
    const size_t slash = path.find_last_of("/\\");
    const std::string dir  = slash == std::string::npos ? "." : path.substr(0, slash);
    const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    JAH_TRY {
        Ogre::ResourceGroupManager &rgm = Ogre::ResourceGroupManager::getSingleton();
        static const char *kGroup = "Jahshaka";
        if (!rgm.resourceGroupExists(kGroup)) rgm.createResourceGroup(kGroup, false);
        if (!mTextureDirs.count(dir)) {
            rgm.addResourceLocation(dir, "FileSystem", kGroup, false);
            mTextureDirs.insert(dir);
        }
        if (!rgm.resourceExists(kGroup, file)) { mError = "loadTexture: file not found: " + path; return 0; }
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
        {
            // Grayscale files (single-channel jpg/png) decode to an R8 texture and
            // sample red-only — a black/white checker renders black/red. Expand to
            // RGBA on the CPU and upload with CPU-generated mipmaps instead.
            //
            // load2, not load(file, group): load() picks the codec by file
            // extension alone and THROWS on mislabeled files (the old importer
            // wrote PNG bytes under .jpg names — GLB embedded textures), which
            // made every such texture silently render white. load2 validates
            // the extension's codec against the magic bytes and falls back to
            // content sniffing — the same tolerant route TextureGpuManager
            // itself uses for the actual GPU load below.
            Ogre::Image2 probe;
            {
                Ogre::DataStreamPtr stream = rgm.openResource(file, kGroup);
                probe.load2(stream, file);
            }
            const Ogre::PixelFormatGpu pf = probe.getPixelFormat();
            if (Ogre::PixelFormatGpuUtils::getNumberOfComponents(pf) == 1 &&
                !Ogre::PixelFormatGpuUtils::isCompressed(pf)) {
                const Ogre::uint32 w = probe.getWidth(), h = probe.getHeight();
                Ogre::Image2 *rgba = new Ogre::Image2();
                rgba->createEmptyImage(w, h, 1u, Ogre::TextureTypes::Type2D,
                                       srgb ? Ogre::PFG_RGBA8_UNORM_SRGB : Ogre::PFG_RGBA8_UNORM,
                                       Ogre::PixelFormatGpuUtils::getMaxMipmapCount(w, h));
                for (Ogre::uint32 y = 0; y < h; ++y)
                    for (Ogre::uint32 x = 0; x < w; ++x) {
                        Ogre::ColourValue c = probe.getColourAt(x, y, 0);
                        c.g = c.b = c.r; c.a = 1.0f;
                        rgba->setColourAt(c, x, y, 0);
                    }
                rgba->generateMipmaps(srgb, Ogre::Image2::FILTER_BILINEAR);
                Ogre::TextureGpu *tex = tm->createTexture(processUniqueName("gray"),
                                                          Ogre::GpuPageOutStrategy::Discard, 0,
                                                          Ogre::TextureTypes::Type2D);
                tex->setResolution(w, h);
                tex->setNumMipmaps(rgba->getNumMipmaps());
                tex->setPixelFormat(rgba->getPixelFormat());
                tex->scheduleTransitionTo(Ogre::GpuResidency::Resident, rgba, true);   // deletes rgba
                tex->waitForData();
                TextureRec rec; rec.texture = tex; rec.path = path;
                mTextures[++mNextTextureId] = rec;
                return mNextTextureId;
            }
        }
        Ogre::uint32 flags = Ogre::TextureFlags::AutomaticBatching;
        if (srgb) flags |= Ogre::TextureFlags::PrefersLoadingFromFileAsSRGB;
        // Alias by full path so the same file name in two folders stays distinct.
        Ogre::TextureGpu *tex = tm->createOrRetrieveTexture(file, path, Ogre::GpuPageOutStrategy::Discard,
                                                            flags, Ogre::TextureTypes::Type2D, kGroup,
                                                            Ogre::TextureFilter::TypeGenerateDefaultMipmaps);
        if (!tex) { mError = "loadTexture: could not create texture for " + path; return 0; }
        tex->scheduleTransitionTo(Ogre::GpuResidency::Resident);
        tex->waitForData();
        TextureRec rec; rec.texture = tex; rec.path = path;
        mTextures[++mNextTextureId] = rec;
        return mNextTextureId;
    } JAH_CATCH(mError, 0);
}

TextureId OgreScene::createTexture(unsigned w, unsigned h, const unsigned char *rgba, bool srgb) {
    if (!w || !h || !rgba) { mError = "createTexture: empty image"; return 0; }
    JAH_TRY {
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
        const std::string name = processUniqueName("pixels");
        // ManualTexture (non-batched) on purpose: setSkyCubemap copyTo's these
        // into cubemap faces, which batched pool slices cannot do. KNOWN macOS
        // DEFECT: non-batched base maps sample the wrong data under MoltenVK
        // (pbr_texture_scale_tiles_uvs; file-loaded/batched textures are fine) —
        // fixing it means teaching the cubemap path to copy from pool slices,
        // then batching these like loadTexture does.
        Ogre::TextureGpu *tex = tm->createTexture(name, Ogre::GpuPageOutStrategy::Discard,
                                                  Ogre::TextureFlags::ManualTexture, Ogre::TextureTypes::Type2D);
        tex->setResolution(w, h);
        tex->setNumMipmaps(1u);
        tex->setPixelFormat(srgb ? Ogre::PFG_RGBA8_UNORM_SRGB : Ogre::PFG_RGBA8_UNORM);
        // IMMEDIATE residency (_transitionTo), and NO explicit notifyDataIsReady:
        // for a ManualTexture _transitionTo(Resident) calls notifyDataIsReady
        // ITSELF (OgreTextureGpu.cpp:600). A second call underflows the uint8
        // mDataPreparationsPending to 255, isDataReady() then never turns true,
        // and anything that waits on the texture — InstantRadiosity's
        // downloadTexture under GI, Image2::convertFromTexture — spins in
        // waitForData forever (found by the GI churn test hanging).
        tex->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        Ogre::StagingTexture *staging = tm->getStagingTexture(w, h, 1u, 1u, tex->getPixelFormat());
        staging->startMapRegion();
        Ogre::TextureBox box = staging->mapRegion(w, h, 1u, 1u, tex->getPixelFormat());
        for (unsigned y = 0; y < h; ++y) std::memcpy(box.at(0, y, 0), rgba + size_t(y) * w * 4u, size_t(w) * 4u);
        staging->stopMapRegion();
        staging->upload(box, tex, 0, nullptr, nullptr, true);
        tm->removeStagingTexture(staging);
        TextureRec rec; rec.texture = tex; rec.path = "";
        mTextures[++mNextTextureId] = rec;
        return mNextTextureId;
    } JAH_CATCH(mError, 0);
}

void OgreScene::releaseTextureRec(const TextureRec &rec) {
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    // A decal-atlas slice is shared across every scene in the process: only the
    // last user's release frees it (OgreDecals.cpp). Destroying it outright here
    // would leave the other scenes' Decals pointing at a dead TextureGpu.
    if (rec.decal) { detail::releaseDecalTexture(tm, rec.decalKind, rec.texture); return; }
    tm->destroyTexture(rec.texture);
}

bool OgreScene::destroyTexture(TextureId id) {
    auto it = mTextures.find(id);
    if (it == mTextures.end()) return false;
    JAH_TRY {
        invalidateGiCaches();   // BEFORE the texture dies: IR caches images by TextureGpu*
        releaseTextureRec(it->second);
        mTextures.erase(it);
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::setPbrTexture(MaterialId mat, PbrTextureSlot slot, TextureId texId) {
    auto mit = mMaterials.find(mat);
    if (mit == mMaterials.end() || mit->second.unlit) { mError = "setPbrTexture: not a PBR material"; return false; }
    Ogre::TextureGpu *tex = nullptr;
    if (texId) { auto tit = mTextures.find(texId); if (tit == mTextures.end()) { mError = "setPbrTexture: unknown texture"; return false; } tex = tit->second.texture; }
    JAH_TRY {
        auto *db = static_cast<Ogre::HlmsPbsDatablock *>(hlmsFor(mit->second)->getDatablock(Ogre::IdString(mit->second.datablockName)));
        if (!db) return false;
        Ogre::PbsTextureTypes unit = Ogre::PBSM_DIFFUSE;
        switch (slot) {
        case PbrTextureSlot::Albedo:    unit = Ogre::PBSM_DIFFUSE;   break;
        case PbrTextureSlot::Normal:    unit = Ogre::PBSM_NORMAL;    break;
        case PbrTextureSlot::Metalness: unit = Ogre::PBSM_METALLIC;  break;
        case PbrTextureSlot::Roughness: unit = Ogre::PBSM_ROUGHNESS; break;
        case PbrTextureSlot::Emissive:  unit = Ogre::PBSM_EMISSIVE;  break;
        }
        Ogre::HlmsSamplerblock sampler;
        sampler.mU = Ogre::TAM_WRAP; sampler.mV = Ogre::TAM_WRAP;
        // Anisotropy must stay 1 while min/mag/mip are FO_LINEAR: Ogre warns, and
        // NVIDIA ignores the mismatch, but Metal (MoltenVK) applies maxAnisotropy
        // regardless of filter mode and averages the whole texture into every
        // texel (caught by pbr_texture_scale_tiles_uvs on the first macOS run).
        // Real anisotropic filtering = FO_ANISOTROPIC on all three filters plus a
        // pixel-suite recalibration — a deliberate visual change, not a default.
        sampler.mMaxAnisotropy = 1; sampler.mMipFilter = Ogre::FO_LINEAR;
        db->setTexture(static_cast<Ogre::uint8>(unit), tex, &sampler);
        return true;
    } JAH_CATCH(mError, false);
}

// ---- Overlay primitives ----
MaterialId OgreScene::createUnlitMaterial(const Colour &c, bool depthTest, bool wireframe) {
    JAH_TRY {
        MaterialRec rec; rec.datablockName = processUniqueName("unlit"); rec.unlit = true; rec.onTop = !depthTest;
        auto *hlmsUnlit = static_cast<Ogre::HlmsUnlit *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));
        Ogre::HlmsMacroblock macro;
        macro.mDepthCheck = depthTest;
        macro.mDepthWrite = depthTest;
        macro.mCullMode = Ogre::CULL_NONE;
        if (wireframe) macro.mPolygonMode = Ogre::PM_WIREFRAME;
        Ogre::HlmsBlendblock blend;
        if (c.a < 0.999f) blend.setBlendType(Ogre::SBT_TRANSPARENT_ALPHA);
        auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->createDatablock(
            Ogre::IdString(rec.datablockName), rec.datablockName, macro, blend, Ogre::HlmsParamVec()));
        db->setUseColour(true);
        db->setColour(toOgre(c));
        mMaterials[++mNextMaterialId] = rec;
        return mNextMaterialId;
    } JAH_CATCH(mError, 0);
}

MaterialId OgreScene::createOutlineMaterial(const Colour &c) {
    JAH_TRY {
        MaterialRec rec; rec.datablockName = processUniqueName("outline"); rec.unlit = true;
        auto *hlmsUnlit = static_cast<Ogre::HlmsUnlit *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));
        Ogre::HlmsMacroblock macro;
        // Inverted hull: cull FRONT faces so only the shell's far side shows,
        // forming a silhouette band around the (slightly smaller) original.
        macro.mCullMode = Ogre::CULL_ANTICLOCKWISE;
        macro.mDepthCheck = true;
        macro.mDepthWrite = false;
        auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->createDatablock(
            Ogre::IdString(rec.datablockName), rec.datablockName, macro, Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
        db->setUseColour(true);
        db->setColour(toOgre(c));
        mMaterials[++mNextMaterialId] = rec;
        return mNextMaterialId;
    } JAH_CATCH(mError, 0);
}

bool OgreScene::setUnlitMaterial(MaterialId id, const Colour &c) {
    auto it = mMaterials.find(id);
    if (it == mMaterials.end() || !it->second.unlit) return false;
    JAH_TRY {
        auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsFor(it->second)->getDatablock(Ogre::IdString(it->second.datablockName)));
        if (!db) return false;
        db->setColour(toOgre(c));
        return true;
    } JAH_CATCH(mError, false);
}

}}}  // namespace jahshaka::engine::detail
