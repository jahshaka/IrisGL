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
    // The hemisphere pair, expressed EXACTLY in the SH basis the backend now
    // runs on: f(n) = lerp(lower, upper, n.y * 0.5 + 0.5)
    //              = (upper + lower)/2  +  (upper - lower)/2 * n.y,
    // i.e. the constant band and the y term, nothing else. No approximation.
    //
    // The 1/pi on the EQUAL-colour case, and only there. HlmsPbs' two ambient
    // paths do not agree with each other by a factor of pi, and which one a
    // caller got used to depend on exactly this test:
    //   * upper != lower selected AmbientHemisphere, whose colours feed
    //     envColourD and are multiplied by pi against a kD that already carries
    //     1/pi (200.BRDFs_piece_ps.any:305) — i.e. a mean RADIANCE;
    //   * upper == lower selected AmbientFixed, which does
    //     `finalColour += ambient * kD` with no pi — i.e. pi times darker for the
    //     same numbers.
    // (OgreHlmsPbs.cpp:1698, the AmbientAutoNormal branch.) Every caller in the
    // tree was written against whichever of the two it happened to hit, so the
    // conversion reproduces the split rather than picking a side: a flat ambient
    // keeps its old (dark) meaning, a hemisphere pair keeps its old (radiance)
    // one. Sky-driven ambient never comes through here — the mirror pushes
    // radiance-unit SH straight to setAmbientSh.
    // FINDING for the lead: that pi cliff at upper == lower is Ogre's, not ours,
    // and it is now the only reason this scale factor exists.
    const bool flat = upper.r == lower.r && upper.g == lower.g && upper.b == lower.b;
    const float kFlat = flat ? 1.0f / 3.14159265358979f : 1.0f;
    const float c0[3] = { (upper.r + lower.r) * 0.5f * kFlat,
                          (upper.g + lower.g) * 0.5f * kFlat,
                          (upper.b + lower.b) * 0.5f * kFlat };
    const float c1[3] = { (upper.r - lower.r) * 0.5f * kFlat,
                          (upper.g - lower.g) * 0.5f * kFlat,
                          (upper.b - lower.b) * 0.5f * kFlat };
    float sh[27] = { 0 };
    for (int c = 0; c < 3; ++c) { sh[c] = c0[c]; sh[3 + c] = c1[c]; }
    setAmbientSh(sh);
}

void OgreScene::setAmbientSh(const float sh[27]) {
    JAH_TRY {
        // HlmsPbs does NOT evaluate the SH basis on the world normal. It uses
        //     wsNormal = mul( passBuf.invViewMatCubemap, normal ); wsNormal.x = -wsNormal.x;
        // (AmbientLighting_piece_ps.any) — the left-handed cubemap frame with X
        // flipped on top, which works out to the world frame rotated 180 degrees
        // about Y: (x, y, z) -> (-x, y, -z). Under that rotation the basis terms
        // {1, y, z, x, xy, yz, 3z^2-1, zx, x^2-y^2} pick up the signs below, so
        // the coefficients a caller gives in WORLD axes are multiplied by them to
        // land where the caller meant. VERIFIED by ambient_sh_lights_world_axes
        // (tests/engine): each band lights the face of a cube it names.
        static const float kAxisSign[9] = { 1, 1, -1, -1, -1, -1, 1, 1, 1 };
        Ogre::Vector3 coeffs[9];
        for (int i = 0; i < 9; ++i)
            coeffs[i] = Ogre::Vector3(sh[i * 3 + 0], sh[i * 3 + 1], sh[i * 3 + 2]) * kAxisSign[i];
        mSceneMgr->setSphericalHarmonics(coeffs);
        // The two-colour ambient still drives envmapScale (it rides
        // ambientUpperHemi.w) and is what a non-SH Hlms would read; keep it at
        // the SH constant band so nothing reads stale colours.
        //
        // envFeatures = 0, NOT the 0xffffffff default: that default turns on
        // EnvFeatures_DiffuseGiFromReflectionProbe, which adds the reflection
        // cubemap's roughest mip to envColourD as an approximate diffuse GI. We
        // now have the real thing in SH, and the reflection cube is a genuine
        // GGX convolution rather than the face-local box mips it used to be — so
        // leaving the flag on both DOUBLE-COUNTS the ambient and undoes the
        // hemisphere split (a sky lit only below its horizon was lighting
        // upward-facing surfaces at 0.69 instead of 0.04). Ogre's own doc says
        // exactly this: "do not set this flag ... because the diffuse GI is
        // already gathered from another source of information".
        const Ogre::ColourValue flat(sh[0], sh[1], sh[2], 1.0f);
        mSceneMgr->setAmbientLight(flat, flat, Ogre::Vector3::UNIT_Y, 1.0f, 0u);
    } JAH_CATCH(mError, );
}

bool OgreScene::removeNode(NodeId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return false;
    JAH_TRY {
        const bool hadDecal = it->second.decal != nullptr;
        releaseNode(it->second);
        mNodes.erase(it);
        // The last decal leaving must clear the SceneManager's atlas bindings,
        // which is what drops the decal code back out of every PBS shader.
        if (hadDecal) refreshDecalBindings();
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
            // Ogre budgets ONE forward area light of each kind and silently
            // drops the rest — a scene's second area light renders nothing
            // until this runs. Same lazy arm point, same reasoning.
            lightextras::armAreaLightBudgets(mRoot);
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

        // IES profile and area-light mask: assigned ONLY when the requested path
        // actually changed. This function runs for every light on every mirror
        // sync (60 Hz); LightProfiles::build() recreates and re-uploads a GPU
        // texture, and a mask costs a decode + resize + mip chain + upload.
        //
        // Honesty about what the renderer does with these — the host UI says the
        // same thing, because there is no engine signal for either:
        //   * a profile shapes SPOT lights always, POINT lights only while they
        //     cast no shadows (a shadow-casting point light is shaded from the
        //     pass buffer, whose point loop has no profile term), and never
        //     directional or area lights;
        //   * a mask applies to the area-light APPROXIMATION only — LTC
        //     ("accurate") ignores it — so accurate mode drops it here rather
        //     than leaving a stale slice bound.
        {
            const bool profileApplies =
                d.type == LightType::Spot ||
                (d.type == LightType::Point && !d.castShadows);
            const std::string wantProfile = profileApplies ? d.iesProfilePath : std::string();
            if (wantProfile != n.lightProfilePath) {
                std::string err;
                if (lightextras::assignProfile(mRoot, L, wantProfile, err)) {
                    n.lightProfilePath = wantProfile;
                } else {
                    mError = err;
                    // Remember the REQUEST anyway: a failing path must not be
                    // retried (and re-logged) every single frame.
                    n.lightProfilePath = wantProfile;
                }
            }

            const bool maskApplies = d.type == LightType::Area && !d.accurate;
            const std::string wantMask = maskApplies ? d.texturePath : std::string();
            if (wantMask != n.lightMaskPath) {
                std::string err;
                if (!lightextras::assignAreaMask(mRoot, L, wantMask, err)) mError = err;
                n.lightMaskPath = wantMask;
            }
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
        // A recreated light starts with no profile and no mask: forget what the
        // dead one carried, or the next setLight would skip re-assigning it.
        it->second.lightProfilePath.clear();
        it->second.lightMaskPath.clear();
        if (it->second.lightNode) { mSceneMgr->destroySceneNode(it->second.lightNode); it->second.lightNode = nullptr; }
        invalidateGiCaches();   // a vanished light must stop bouncing (VCT re-injects)
        return true;
    } JAH_CATCH(mError, false);
}

Ogre::SceneManager *OgreScene::sceneManager() const { return mSceneMgr; }

void OgreScene::destroy() {
    if (!mSceneMgr) return;
    JAH_TRY {
        teardownGi();   // VPL lights die while the SceneManager is still alive
        // The atmosphere destroys its Rectangle2D THROUGH the SceneManager, so it
        // has to go while that is still alive (teardown law: components, then the
        // manager).
        destroyAtmosphere();
        destroySky();   // also unbinds + destroys the reflection cubemap
        for (auto &kv : mNodes) releaseNode(kv.second);
        mNodes.clear();
        for (auto &kv : mMaterials) {
            Ogre::Hlms *hlms = hlmsFor(kv.second);
            if (hlms->getDatablock(Ogre::IdString(kv.second.datablockName)))
                hlms->destroyDatablock(Ogre::IdString(kv.second.datablockName));
        }
        mMaterials.clear();
        for (auto &kv : mTextures) releaseTextureRec(kv.second);
        mTextures.clear();
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
        // Only GI-participating (lit) geometry invalidates — detaching a selection
        // outline or wire overlay must not trigger a re-voxelize. BEFORE the
        // destroy: the voxelizer/IR hold raw pointers into the dying geometry.
        if (n.item->getVisibilityFlags() & kGiGeometryBit) invalidateGiCaches();
        n.item->detachFromParent(); mSceneMgr->destroyItem(n.item); n.item = nullptr;
    }
    n.meshRef = 0; n.materialRef = 0;
}

void OgreScene::releaseNode(Node &n) {
    // Order: renderable off the node -> item (drops the datablock link and one
    // mesh ref) -> datablock -> node -> our mesh ref -> the mesh itself.
    releaseBillboards(n);
    // Invalidate BEFORE anything dies (IR frees its by-pointer caches inside):
    // VCT holds the raw Item*, IR caches the mesh's VAO and any node-owned mesh.
    if (n.mesh || (n.item && (n.item->getVisibilityFlags() & kGiGeometryBit)))
        invalidateGiCaches();
    if (n.item)  { n.item->detachFromParent();  mSceneMgr->destroyItem(n.item);   n.item = nullptr; }
    n.meshRef = 0; n.materialRef = 0;
    // The internal light child must go before the reparent loop below would leak it to root.
    if (n.light) { n.light->detachFromParent(); mSceneMgr->destroyLight(n.light); n.light = nullptr; }
    if (n.lightNode) { mSceneMgr->destroySceneNode(n.lightNode); n.lightNode = nullptr; }
    // Same for the decal's internal child. Note releaseDecal only tears the
    // objects down; the SceneManager's atlas bindings are refreshed by the
    // caller (removeNode) once, after the node is gone.
    releaseDecal(n);
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
