// Particles: the BillboardSet2 mirror of the document's particle systems.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

bool OgreScene::createBillboardSet(NodeId id, TextureId texId, bool additiveBlend,
                                   unsigned capacity) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) { mError = "createBillboardSet: unknown node"; return false; }
    Ogre::TextureGpu *tex = nullptr;
    if (texId) {
        auto tit = mTextures.find(texId);
        if (tit == mTextures.end()) { mError = "createBillboardSet: unknown texture"; return false; }
        tex = tit->second.texture;
    }
    JAH_TRY {
        Node &n = it->second;
        releaseBillboards(n);
        // Legacy particle pass: depth test on, depth write off; additive is
        // (SRC_ALPHA, ONE), otherwise plain alpha blending.
        const std::string dbName = processUniqueName("billboards");
        auto *hlmsUnlit = static_cast<Ogre::HlmsUnlit *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));
        Ogre::HlmsMacroblock macro;
        macro.mDepthCheck = true; macro.mDepthWrite = false; macro.mCullMode = Ogre::CULL_NONE;
        Ogre::HlmsBlendblock blend;
        if (additiveBlend) {
            blend.mSourceBlendFactor = Ogre::SBF_SOURCE_ALPHA;
            blend.mDestBlendFactor   = Ogre::SBF_ONE;
        } else {
            blend.setBlendType(Ogre::SBT_TRANSPARENT_ALPHA);
        }
        auto *db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->createDatablock(
            Ogre::IdString(dbName), dbName, macro, blend, Ogre::HlmsParamVec()));
        db->setUseColour(true);
        db->setColour(Ogre::ColourValue::White);
        if (tex) {
            Ogre::HlmsSamplerblock sampler;
            sampler.mU = Ogre::TAM_CLAMP; sampler.mV = Ogre::TAM_CLAMP;
            sampler.mMipFilter = Ogre::FO_LINEAR;
            db->setTexture(0, tex, &sampler);
        }
        Ogre::BillboardSet *set = mSceneMgr->createBillboardSet2();
        set->setParticleQuota(std::max(1u, capacity));   // aligned up internally
        // Legacy rotates the quad's vertices around the view axis (not the UVs).
        set->setRotationType(Ogre::ParticleRotationType::Vertex);
        set->setMaterialName(dbName, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
        set->init(mRoot->getRenderSystem()->getVaoManager());
        n.billboards = set;
        n.billboardDatablockName = dbName;
        n.billboardCapacity = std::max(1u, capacity);
        // Visibility follows the node: the caller pushes it via setNodeVisible
        // (the mirror does so every frame); a fresh set starts visible.
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::setBillboards(NodeId id, const BillboardInstance *data, size_t count) {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.billboards) {
        mError = "setBillboards: node has no billboard set"; return false;
    }
    if (!data && count) { mError = "setBillboards: null data"; return false; }
    JAH_TRY {
        Node &n = it->second;
        const size_t want = std::min(count, size_t(n.billboardCapacity));
        while (n.billboardHandles.size() < want) {
            Ogre::Billboard b = n.billboards->allocBillboard();
            if (b.mHandle == Ogre::ParticleSystemDef::InvalidHandle) break;
            n.billboardHandles.push_back(b.mHandle);
        }
        while (n.billboardHandles.size() > want) {
            n.billboards->deallocBillboard(n.billboardHandles.back());
            n.billboardHandles.pop_back();
        }
        for (size_t i = 0; i < n.billboardHandles.size(); ++i) {
            const BillboardInstance &src = data[i];
            // The shader spans the quad pos +/- dim: dim is the HALF extent.
            const float half = std::max(0.0f, src.size * 0.5f);
            // Rotation is packed snorm * pi on upload: normalise to [-pi, pi].
            float rot = std::fmod(src.rotationRadians, 2.0f * Ogre::Math::PI);
            if (rot >  Ogre::Math::PI) rot -= 2.0f * Ogre::Math::PI;
            if (rot < -Ogre::Math::PI) rot += 2.0f * Ogre::Math::PI;
            Ogre::Billboard b(n.billboardHandles[i], n.billboards);
            b.set(toOgre(src.position), Ogre::Vector3::NEGATIVE_UNIT_Z,
                  Ogre::Vector2(half, half), toOgre(src.colour), Ogre::Radian(rot));
        }
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::destroyBillboardSet(NodeId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.billboards) return false;
    JAH_TRY { releaseBillboards(it->second); return true; } JAH_CATCH(mError, false);
}

void OgreScene::releaseBillboards(Node &n) {
    if (n.billboards) {
        // Detaches, releases the GPU buffers via the live VaoManager, deletes.
        mSceneMgr->destroyBillboardSet2(n.billboards);
        n.billboards = nullptr;
        n.billboardHandles.clear();
        n.billboardCapacity = 0;
    }
    if (!n.billboardDatablockName.empty()) {
        auto *hlmsUnlit = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
        if (hlmsUnlit->getDatablock(Ogre::IdString(n.billboardDatablockName)))
            hlmsUnlit->destroyDatablock(Ogre::IdString(n.billboardDatablockName));
        n.billboardDatablockName.clear();
    }
}

}}}  // namespace jahshaka::engine::detail
