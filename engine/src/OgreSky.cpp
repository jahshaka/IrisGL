// Sky geometry (equirect sphere / cubemap faces) and the IBL reflection cubemap
// derived from it.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

bool OgreScene::setSky(SkyMode mode, TextureId texId) {
    JAH_TRY {
        if (mode == SkyMode::NoSky) { destroySky(); return true; }
        if (mode == SkyMode::Cubemap) { mError = "setSky: cubemap skies are not supported yet (equirectangular only)"; return false; }
        auto it = mTextures.find(texId);
        if (it == mTextures.end()) { mError = "setSky: unknown texture"; return false; }
        // A cubemap sky may already occupy mSkyNode (six quads with their own
        // datablocks and no mSkyDatablockName) — replace it rather than inherit
        // its node, or the lookup below derefs a null datablock.
        if (mSkyNode && !mSkyFaces.empty()) destroySky();
        if (!mSkyNode) {
            mSkyMeshName = processUniqueName("skysphere");
            mSkyMesh = buildSkySphere(mSkyMeshName);
            auto *hlmsUnlit = static_cast<Ogre::HlmsUnlit *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));
            mSkyDatablockName = processUniqueName("sky");
            Ogre::HlmsMacroblock macro;
            macro.mDepthCheck = false; macro.mDepthWrite = false; macro.mCullMode = Ogre::CULL_NONE;
            auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->createDatablock(
                Ogre::IdString(mSkyDatablockName), mSkyDatablockName, macro, Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
            db->setUseColour(true);
            db->setColour(Ogre::ColourValue::White);
            mSkyItem = mSceneMgr->createItem(mSkyMesh, Ogre::SCENE_DYNAMIC);
            mSkyItem->setDatablock(db);
            mSkyItem->setRenderQueueGroup(0);
            mSkyItem->setCastShadows(false);
            mSkyItem->setVisibilityFlags(kVisibleBit);   // never GI geometry
            mSkyNode = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode(Ogre::SCENE_DYNAMIC);
            mSkyNode->attachObject(mSkyItem);
        }
        auto *hlmsUnlit = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->getDatablock(Ogre::IdString(mSkyDatablockName)));
        Ogre::HlmsSamplerblock sampler;
        sampler.mU = Ogre::TAM_WRAP; sampler.mV = Ogre::TAM_CLAMP; sampler.mMipFilter = Ogre::FO_LINEAR;
        db->setTexture(0, it->second.texture, &sampler);
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
        destroySky();
        mSkyNode = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->createChildSceneNode(Ogre::SCENE_DYNAMIC);
        auto *hlmsUnlit = static_cast<Ogre::HlmsUnlit *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));
        // Each face: a unit quad at distance 1 along its axis, facing inward. Order +X,-X,+Y,-Y,+Z,-Z.
        // (right, up) vectors chosen so an image's top stays up and left stays left when viewed from inside.
        const float ax[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        const float rt[6][3] = {{0,0,-1},{0,0,1},{1,0,0},{1,0,0},{1,0,0},{-1,0,0}};
        const float up[6][3] = {{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
        for (int f = 0; f < 6; ++f) {
            MeshData d;
            const float *a = ax[f], *r = rt[f], *u = up[f];
            const float corners[4][2] = {{-1,-1},{1,-1},{1,1},{-1,1}};   // (right, up) multipliers
            for (int c = 0; c < 4; ++c) {
                for (int k = 0; k < 3; ++k) d.positions.push_back(a[k] + r[k] * corners[c][0] + u[k] * corners[c][1]);
                for (int k = 0; k < 3; ++k) d.normals.push_back(-a[k]);
                d.uvs.push_back(corners[c][0] > 0 ? 1.0f : 0.0f);
                d.uvs.push_back(corners[c][1] > 0 ? 0.0f : 1.0f);   // image row 0 at the top
            }
            for (unsigned i : { 0u, 2u, 1u, 0u, 3u, 2u }) d.indices.push_back(i);   // inward winding
            const std::string meshName = processUniqueName("skyface");
            Ogre::MeshPtr mesh = buildMeshV2(meshName, d);
            const std::string dbName = processUniqueName("skyface");
            Ogre::HlmsMacroblock macro;
            macro.mDepthCheck = false; macro.mDepthWrite = false; macro.mCullMode = Ogre::CULL_NONE;
            auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->createDatablock(
                Ogre::IdString(dbName), dbName, macro, Ogre::HlmsBlendblock(), Ogre::HlmsParamVec()));
            db->setUseColour(true); db->setColour(Ogre::ColourValue::White);
            Ogre::HlmsSamplerblock sampler;
            sampler.mU = Ogre::TAM_CLAMP; sampler.mV = Ogre::TAM_CLAMP; sampler.mMipFilter = Ogre::FO_LINEAR;
            db->setTexture(0, tex[f], &sampler);
            Ogre::Item *item = mSceneMgr->createItem(mesh, Ogre::SCENE_DYNAMIC);
            item->setDatablock(db);
            item->setRenderQueueGroup(0);
            item->setCastShadows(false);
            item->setVisibilityFlags(kVisibleBit);   // never GI geometry
            mSkyNode->attachObject(item);
            mSkyFaces.push_back({ item, mesh, meshName, dbName });
        }
        // Environment reflections: the same six faces become a mipped cubemap on
        // every PBR datablock's reflection slot, so metals and glass mirror the
        // sky the way the legacy matcap/refraction shaders faked it.
        if (tex[0]->getWidth() == tex[1]->getWidth())
            buildReflectionCubemap(tex);
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
        buildReflectionCubemap(tex);
        return true;
    } JAH_CATCH(mError, false);
}

void OgreScene::buildReflectionCubemap(Ogre::TextureGpu *const tex[6]) {
    destroyReflection();
    const Ogre::uint32 w = tex[0]->getWidth(), h = tex[0]->getHeight();
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    Ogre::TextureGpu *cube = tm->createTexture(
        processUniqueName("skyrefl"), Ogre::GpuPageOutStrategy::Discard,
        Ogre::TextureFlags::RenderToTexture | Ogre::TextureFlags::AllowAutomipmaps,
        Ogre::TextureTypes::TypeCube);
    cube->setResolution(w, h, 6u);
    cube->setPixelFormat(tex[0]->getPixelFormat());
    cube->setNumMipmaps(Ogre::PixelFormatGpuUtils::getMaxMipmapCount(w, h));
    cube->scheduleTransitionTo(Ogre::GpuResidency::Resident);
    for (int i = 0; i < 6; ++i) {
        Ogre::TextureBox dst = cube->getEmptyBox(0); dst.sliceStart = Ogre::uint32(i); dst.numSlices = 1u;
        tex[i]->copyTo(cube, dst, 0, tex[i]->getEmptyBox(0), 0);
    }
    cube->_autogenerateMipmaps();
    mReflectionTex = cube;
    applyReflectionToAll();
}

void OgreScene::destroyReflection() {
    if (!mReflectionTex) return;
    Ogre::TextureGpu *tex = mReflectionTex;
    mReflectionTex = nullptr;
    applyReflectionToAll();
    mRoot->getRenderSystem()->getTextureGpuManager()->destroyTexture(tex);
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

void OgreScene::followCamera(const Ogre::Vector3 &camPos, float farClip) {
    if (!mSkyNode) return;
    mSkyNode->setPosition(camPos);
    const float r = std::max(1.0f, farClip * 0.5f);
    mSkyNode->setScale(r, r, r);
}

void OgreScene::destroySky() {
    // Unbind the reflection cubemap from every datablock before it goes away.
    destroyReflection();
    for (SkyFace &f : mSkyFaces) {
        if (f.item) { f.item->detachFromParent(); mSceneMgr->destroyItem(f.item); }
        auto *hlmsUnlit = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (hlmsUnlit->getDatablock(Ogre::IdString(f.dbName))) hlmsUnlit->destroyDatablock(Ogre::IdString(f.dbName));
        f.mesh.reset();
        Ogre::MeshManager &mm = Ogre::MeshManager::getSingleton();
        if (mm.resourceExists(f.meshName)) mm.remove(f.meshName);
    }
    mSkyFaces.clear();
    if (mSkyItem) { mSkyItem->detachFromParent(); mSceneMgr->destroyItem(mSkyItem); mSkyItem = nullptr; }
    if (mSkyNode) { mSceneMgr->destroySceneNode(mSkyNode); mSkyNode = nullptr; }
    if (!mSkyDatablockName.empty()) {
        auto *hlmsUnlit = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (hlmsUnlit->getDatablock(Ogre::IdString(mSkyDatablockName))) hlmsUnlit->destroyDatablock(Ogre::IdString(mSkyDatablockName));
        mSkyDatablockName.clear();
    }
    mSkyMesh.reset();
    if (!mSkyMeshName.empty()) {
        Ogre::MeshManager &mm = Ogre::MeshManager::getSingleton();
        if (mm.resourceExists(mSkyMeshName)) mm.remove(mSkyMeshName);
        mSkyMeshName.clear();
    }
}

Ogre::MeshPtr OgreScene::buildSkySphere(const std::string &name) {
    MeshData d;
    const int rings = 24, segs = 48;
    for (int r = 0; r <= rings; ++r) {
        const float v = float(r) / rings, phi = v * 3.14159265f;
        for (int sIdx = 0; sIdx <= segs; ++sIdx) {
            const float u = float(sIdx) / segs, theta = u * 6.2831853f;
            const float x = std::sin(phi) * std::cos(theta), y = std::cos(phi), z = std::sin(phi) * std::sin(theta);
            d.positions.push_back(x); d.positions.push_back(y); d.positions.push_back(z);
            d.normals.push_back(-x); d.normals.push_back(-y); d.normals.push_back(-z);
            d.uvs.push_back(1.0f - u); d.uvs.push_back(v);
        }
    }
    for (int r = 0; r < rings; ++r) for (int sIdx = 0; sIdx < segs; ++sIdx) {
        const unsigned a = unsigned(r * (segs + 1) + sIdx), b = a + unsigned(segs + 1);
        d.indices.push_back(a); d.indices.push_back(a + 1); d.indices.push_back(b);      // inward winding
        d.indices.push_back(a + 1); d.indices.push_back(b + 1); d.indices.push_back(b);
    }
    return buildMeshV2(name, d);
}

}}}  // namespace jahshaka::engine::detail
