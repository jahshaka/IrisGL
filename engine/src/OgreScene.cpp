// OgreScene core: lifetime, node hierarchy, transforms, visibility, lights and
// the teardown helpers. Meshes, materials, sky, GI and particles live in their
// own translation units.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

OgreScene::OgreScene(Ogre::Root *root, Ogre::SceneManager *sm, const std::string &name,
                     std::string &errorSink)
    : mRoot(root), mSceneMgr(sm), mName(name), mError(errorSink) {}

OgreScene::~OgreScene() { destroy(); }

const std::string &OgreScene::name() const { return mName; }

void OgreScene::setAmbient(const Colour &upper, const Colour &lower) {
    JAH_TRY {
        mSceneMgr->setAmbientLight(toOgre(upper), toOgre(lower), Ogre::Vector3::UNIT_Y);
    } JAH_CATCH(mError, );
}

bool OgreScene::removeNode(NodeId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return false;
    JAH_TRY {
        releaseNode(it->second);
        mNodes.erase(it);
        return true;
    } JAH_CATCH(mError, false);
}

// ---- Hierarchy and transforms ----
NodeId OgreScene::createNode(NodeId parent) {
    JAH_TRY {
        Ogre::SceneNode *p = parent ? node(parent) : nullptr;
        if (parent && !p) { mError = "createNode: unknown parent"; return 0; }
        if (!p) p = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC);
        Node rec; rec.node = p->createChildSceneNode(Ogre::SCENE_DYNAMIC);
        return track(rec);
    } JAH_CATCH(mError, 0);
}

bool OgreScene::setNodeParent(NodeId id, NodeId parent) {
    JAH_TRY {
        Ogre::SceneNode *n = node(id);
        if (!n) { mError = "setNodeParent: unknown node"; return false; }
        Ogre::SceneNode *p = parent ? node(parent) : mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC);
        if (!p) { mError = "setNodeParent: unknown parent"; return false; }
        if (n->getParent() == p) return true;
        if (n->getParent()) n->getParent()->removeChild(n);
        p->addChild(n);
        return true;
    } JAH_CATCH(mError, false);
}

void OgreScene::setNodeTransform(NodeId id, const Vec3 &pos, const Quat &rot, const Vec3 &scale) {
    JAH_TRY {
        if (auto *n = node(id)) {
            n->setPosition(toOgre(pos));
            n->setOrientation(Ogre::Quaternion(rot.w, rot.x, rot.y, rot.z));
            n->setScale(toOgre(scale));
        }
    } JAH_CATCH(mError, );
}

void OgreScene::setNodeVisible(NodeId id, bool visible) {
    JAH_TRY {
        auto it = mNodes.find(id);
        if (it == mNodes.end()) return;
        if (it->second.node) it->second.node->setVisible(visible, true);
        // The billboard set hangs off the STATIC root (world-space positions),
        // not off this node, so the cascade above never reaches it. And
        // setVisible() is USELESS for PFX2 objects: ParticleSystemManager2::
        // _addToRenderQueue tests getVisibilityFlags(), which strips the
        // LAYER_VISIBILITY bit setVisible toggles. Toggle the user flags.
        if (it->second.billboards) it->second.billboards->setVisibilityFlags(visible ? 1u : 0u);
    } JAH_CATCH(mError, );
}

// ---- Lights ----
bool OgreScene::setLight(NodeId id, const LightDesc &d) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) { mError = "setLight: unknown node"; return false; }
    JAH_TRY {
        Node &n = it->second;
        if (!n.light) {
            // The document's convention (IrisGL LightNode::getLightDir): lights shine
            // down their node's -Y. Ogre lights shine down -Z, so the light rides an
            // internal child node pitched -90 about X. Getting this wrong leaves every
            // scene lit near-horizontally: dark viewports and shadows nobody can see.
            n.lightNode = n.node->createChildSceneNode();
            n.lightNode->setOrientation(Ogre::Quaternion(Ogre::Radian(-Ogre::Math::HALF_PI), Ogre::Vector3::UNIT_X));
            n.light = mSceneMgr->createLight();
            n.lightNode->attachObject(n.light);
        }
        Ogre::Light *L = n.light;
        switch (d.type) {
        case LightType::Directional: L->setType(Ogre::Light::LT_DIRECTIONAL); break;
        case LightType::Point:       L->setType(Ogre::Light::LT_POINT); break;
        case LightType::Spot:        L->setType(Ogre::Light::LT_SPOTLIGHT); break;
        // Area lights emit down the light's -Z, exactly like spot/directional,
        // so the -Y child-node convention below already orients them.
        case LightType::Area:
            L->setType(d.accurate ? Ogre::Light::LT_AREA_LTC : Ogre::Light::LT_AREA_APPROX);
            break;
        }
        L->setDiffuseColour(toOgre(d.colour));
        L->setSpecularColour(toOgre(d.colour));
        // Ogre-Next cannot render shadows for area lights (and our shadow node
        // only lists directional/point/spot); never mark them casters.
        L->setCastShadows(d.type == LightType::Area ? false : d.castShadows);
        if (d.type == LightType::Area) {
            L->setRectSize(Ogre::Vector2(std::max(d.rectWidth, 0.01f), std::max(d.rectHeight, 0.01f)));
            L->setDoubleSided(d.doubleSided);
            // HlmsPbs only pays for area lights in scenes that contain one
            // (the LightsAreaApprox/Ltc shader properties are gated on the
            // live light list), but the LTC/BRDF lookup textures must be
            // resident before an area light is drawn. Loading them reserves
            // a texture slot in every pass, so do it lazily, once, here —
            // never for scenes without area lights. The .dds files ship with
            // the staged Common material scripts (registerCommonResources).
            static bool sLtcLoaded = false;
            if (!sLtcLoaded) {
                sLtcLoaded = true;   // even a failed attempt: don't retry every frame
                auto *pbs = static_cast<Ogre::HlmsPbs *>(
                    Ogre::Root::getSingleton().getHlmsManager()->getHlms(Ogre::HLMS_PBS));
                if (pbs) pbs->loadLtcMatrix();
            }
        }
        // HlmsPbs divides diffuse by pi (Lambert BRDF); IrisGL's default shader does
        // not, so matching legacy exposure needs powerScale = intensity * pi. (An
        // earlier 'calibration' removed this while the light DIRECTION mapping was
        // broken — the overexposure it fixed was side-lit faces, not the scale.)
        L->setPowerScale(d.intensity * Ogre::Math::PI);
        if (d.type != LightType::Directional) {
            L->setAttenuationBasedOnRadius(std::max(d.range, 0.01f), 0.01f);
        } else {
            // Ogre's default attenuation (const 0.5, quad 0.5) is never used to
            // SHADE a directional light, but Instant Radiosity attenuates its
            // rays with it — over the tens of units a ray travels from outside
            // the scene that quadratic term crushed every bounce to black.
            // The InstantRadiosity sample sets exactly this: no falloff.
            L->setAttenuation(std::numeric_limits<Ogre::Real>::max(), 1.0f, 0.0f, 0.0f);
        }
        if (d.type == LightType::Spot) {
            const float outer = std::max(1.0f, std::min(d.spotAngleDegrees, 179.0f));
            const float inner = outer * (1.0f - std::min(std::max(d.spotSoftness, 0.0f), 0.99f));
            L->setSpotlightRange(Ogre::Degree(inner), Ogre::Degree(outer), 1.0f);
        }
        // Lights shine down their node's -Y once attached (document convention).
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::removeLight(NodeId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.light) return false;
    JAH_TRY {
        it->second.light->detachFromParent();
        mSceneMgr->destroyLight(it->second.light);
        it->second.light = nullptr;
        if (it->second.lightNode) { mSceneMgr->destroySceneNode(it->second.lightNode); it->second.lightNode = nullptr; }
        return true;
    } JAH_CATCH(mError, false);
}

Ogre::SceneManager *OgreScene::sceneManager() const { return mSceneMgr; }

void OgreScene::destroy() {
    if (!mSceneMgr) return;
    JAH_TRY {
        teardownGi();   // VPL lights die while the SceneManager is still alive
        destroySky();   // also unbinds + destroys the reflection cubemap
        for (auto &kv : mNodes) releaseNode(kv.second);
        mNodes.clear();
        for (auto &kv : mMaterials) {
            Ogre::Hlms *hlms = hlmsFor(kv.second);
            if (hlms->getDatablock(Ogre::IdString(kv.second.datablockName)))
                hlms->destroyDatablock(Ogre::IdString(kv.second.datablockName));
        }
        mMaterials.clear();
        {
            Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
            for (auto &kv : mTextures) tm->destroyTexture(kv.second.texture);
            mTextures.clear();
        }
        Ogre::MeshManager &mm = Ogre::MeshManager::getSingleton();
        for (auto &kv : mMeshes) {
            kv.second.mesh.reset();
            if (mm.resourceExists(kv.second.name)) mm.remove(kv.second.name);
        }
        mMeshes.clear();
        mRoot->destroySceneManager(mSceneMgr);
    } JAH_CATCH(mError, );
    FogHlmsListener::unregisterScene(mSceneMgr);
    mSceneMgr = nullptr;
}

void OgreScene::detachItem(Node &n) {
    if (n.item && n.meshRef) {
        n.item->detachFromParent(); mSceneMgr->destroyItem(n.item); n.item = nullptr;
    }
    n.meshRef = 0; n.materialRef = 0;
}

void OgreScene::releaseNode(Node &n) {
    // Order: renderable off the node -> item (drops the datablock link and one
    // mesh ref) -> datablock -> node -> our mesh ref -> the mesh itself.
    releaseBillboards(n);
    if (n.item)  { n.item->detachFromParent();  mSceneMgr->destroyItem(n.item);   n.item = nullptr; }
    if (n.mesh)  invalidateGiCaches();   // node-owned MeshPtr dies below; IR caches its VAO
    n.meshRef = 0; n.materialRef = 0;
    // The internal light child must go before the reparent loop below would leak it to root.
    if (n.light) { n.light->detachFromParent(); mSceneMgr->destroyLight(n.light); n.light = nullptr; }
    if (n.lightNode) { mSceneMgr->destroySceneNode(n.lightNode); n.lightNode = nullptr; }
    if (n.node) {   // children survive: re-parent them to the root
        Ogre::SceneNode *root = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC);
        while (n.node->numChildren() > 0) {
            Ogre::Node *c = n.node->getChild(0);
            n.node->removeChild(c); root->addChild(c);
        }
    }
    if (!n.datablockName.empty()) {
        auto *hlmsPbs = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS);
        if (hlmsPbs->getDatablock(Ogre::IdString(n.datablockName)))
            hlmsPbs->destroyDatablock(Ogre::IdString(n.datablockName));
        n.datablockName.clear();
    }
    if (n.node) { mSceneMgr->destroySceneNode(n.node); n.node = nullptr; }
    n.mesh.reset();
    if (!n.meshName.empty()) {
        Ogre::MeshManager &mm = Ogre::MeshManager::getSingleton();
        if (mm.resourceExists(n.meshName)) mm.remove(n.meshName);
        n.meshName.clear();
    }
}

Ogre::SceneNode *OgreScene::node(NodeId id) const {
    auto it = mNodes.find(id);
    return it == mNodes.end() ? nullptr : it->second.node;
}

NodeId OgreScene::track(const Node &n) { mNodes[++mNextId] = n; return mNextId; }

}}}  // namespace jahshaka::engine::detail
