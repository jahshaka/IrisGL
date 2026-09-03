// Particles.
//
// TWO systems live here, and they are not the same thing:
//
//   * BillboardSet2 (createBillboardSet / setBillboards / destroyBillboardSet) —
//     externally-simulated quads. The host owns every particle's position and
//     pushes the whole list each frame. Its ONLY remaining caller is the light
//     icon in the scene mirror (one quad per light).
//
//   * ParticleSystemDef + ParticleSystem2 (setParticleSystem / removeParticleSystem)
//     — ParticleFX2's own simulation, added by PARTICLES_FX2_SPEC.md. The host
//     pushes PARAMETERS; emission, forces, colour-over-life, spin and death run
//     inside Ogre, SIMD, on the SceneManager's worker threads, advanced by
//     SceneManager::updateSceneGraph on every renderOneFrame. Nothing in
//     Jahshaka integrates a particle any more.
//
// Both render through the same PFX2 vertex path (RQ 15, geometry generated in
// the vertex shader from a read-only buffer, quads always camera-facing), and
// both need Hlms::_setHasParticleFX2Plugin(true) before shaders are built.
// Only the second needs Plugin_ParticleFX2 loaded: the emitter and affector
// FACTORIES live in the plugin, the rest lives in OgreNextMain.
//
// Lifecycle rules that are not negotiable (all pin-verified):
//   * A definition's quota, emitter list and affector list are FROZEN at init():
//     setParticleQuota asserts !isInitialized(), and adding an emitter after a
//     ParticleSystem2 exists indexes past the per-instance emitter array (sized
//     once in the ParticleSystem2 constructor, walked by the def's emitter count
//     in the update loop). Every such change is a new definition.
//   * There is NO API to destroy one definition. createParticleSystemDef has no
//     counterpart; destroyAllParticleSystems destroys INSTANCES and keeps every
//     def; defs are freed only in ~ParticleSystemManager2, i.e. with the
//     SceneManager. So released defs are hidden and parked on mParticleDefPool
//     keyed by their frozen shape, and the next matching system reuses one.
//   * The instance MUST be attached to a SceneNode: emission dereferences
//     system->getParentNode() with no null check.
//   * Visibility is on the DEFINITION, not the instance and not the node:
//     ParticleSystemManager2::_addToRenderQueue tests the def's visibility
//     flags. (Same trap as billboard sets — MovableObject::setVisible cannot
//     hide either of them.)
#include "EnginePrivate.h"

#include <cstdio>

namespace jahshaka { namespace engine { namespace detail {

// ---- helpers ---------------------------------------------------------------

std::string OgreScene::ParticleTopology::key() const {
    // Only the FROZEN properties belong here. Texture, blend mode, alpha
    // hashing, orientation and rotation type are all mutable in place (the
    // first three on the datablock, the last two on the def), so they must NOT
    // split the recycling pool.
    std::string s = "q" + std::to_string(quotaBucket);
    s += "|e";
    for (int shape : emitterShapes) s += std::to_string(shape) + ",";
    s += "|a";
    for (int kind : affectorKinds) s += std::to_string(kind) + ",";
    return s;
}

namespace {

unsigned quotaBucketFor(unsigned requested) {
    const unsigned want = std::max(1u, std::min<unsigned>(requested, kMaxParticleQuota));
    for (unsigned b : kParticleQuotaBuckets)
        if (want <= b) return b;
    return kMaxParticleQuota;
}

const char *emitterFactoryName(ParticleEmitterShape s) {
    switch (s) {
    case ParticleEmitterShape::Point:           return "Point";
    case ParticleEmitterShape::Box:             return "Box";
    case ParticleEmitterShape::Cylinder:        return "Cylinder";
    case ParticleEmitterShape::Ellipsoid:       return "Ellipsoid";
    case ParticleEmitterShape::HollowEllipsoid: return "HollowEllipsoid";
    case ParticleEmitterShape::Ring:            return "Ring";
    }
    return "Point";
}

const char *affectorFactoryName(ParticleAffectorDesc::Kind k) {
    switch (k) {
    case ParticleAffectorDesc::Kind::ColourKeys:     return "ColourInterpolator";
    case ParticleAffectorDesc::Kind::ScaleKeys:      return "ScaleInterpolator";
    case ParticleAffectorDesc::Kind::Rotator:        return "Rotator";
    case ParticleAffectorDesc::Kind::LinearForce:    return "LinearForce";
    case ParticleAffectorDesc::Kind::Turbulence:     return "DirectionRandomiser";
    case ParticleAffectorDesc::Kind::DeflectorPlane: return "DeflectorPlane";
    }
    return "LinearForce";
}

Ogre::ParticleType::ParticleType toOgreParticleType(ParticleOrientation o) {
    switch (o) {
    case ParticleOrientation::Point:               return Ogre::ParticleType::Point;
    case ParticleOrientation::OrientedCommon:      return Ogre::ParticleType::OrientedCommon;
    case ParticleOrientation::OrientedSelf:        return Ogre::ParticleType::OrientedSelf;
    case ParticleOrientation::PerpendicularCommon: return Ogre::ParticleType::PerpendicularCommon;
    case ParticleOrientation::PerpendicularSelf:   return Ogre::ParticleType::PerpendicularSelf;
    }
    return Ogre::ParticleType::Point;
}

std::string vec3Param(const Vec3 &v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%g %g %g", double(v.x), double(v.y), double(v.z));
    return buf;
}
std::string realParam(float f) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%g", double(f));
    return buf;
}
std::string colourParam(const Colour &c) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%g %g %g %g",
                  double(c.r), double(c.g), double(c.b), double(c.a));
    return buf;
}

/// Normalises up to 6 authored keys into the 6 stages the interpolator affectors
/// expect. THE RULE (OgreColourInterpolatorAffector2.cpp:82-98, the identical
/// loop in the scale one): stage j applies while `particleTime >= timeAdj[j]`,
/// the last matching stage wins, and every stage interpolates towards stage j+1.
/// So the stages must be strictly ascending, and every stage past the authored
/// keys must sit ABOVE 1 so it never fires — which is exactly why the affectors
/// are neutral at their defaults (times 1, 2, 3, 4, 5, 6). The tail here repeats
/// the last authored value so the final segment is constant instead of drifting
/// into the constructor's transparent grey.
template <typename T>
void fillInterpStages(const float *srcTimes, const T *srcValues, unsigned count,
                      float outTimes[6], T outValues[6]) {
    const unsigned n = std::min(count, 6u);
    float prev = -1.0f;
    for (unsigned i = 0; i < n; ++i) {
        float t = srcTimes[i];
        if (t <= prev) t = prev + 1e-4f;    // strictly ascending, always
        outTimes[i] = t;
        outValues[i] = srcValues[i];
        prev = t;
    }
    float tail = std::max(prev, 1.0f);
    for (unsigned i = n; i < 6; ++i) {
        tail += 1.0f;
        outTimes[i] = tail;
        outValues[i] = n ? srcValues[n - 1] : T();
    }
}

}  // namespace

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

// ---- Particles: the engine simulates (PARTICLES_FX2_SPEC.md) ---------------

std::string OgreScene::ensureParticleDatablock(Node &n, const ParticleSystemDesc &d) {
    Ogre::TextureGpu *tex = nullptr;
    if (d.texture) {
        auto tit = mTextures.find(d.texture);
        if (tit == mTextures.end()) { mError = "setParticleSystem: unknown texture"; return {}; }
        tex = tit->second.texture;
    }
    auto *hlmsUnlit = static_cast<Ogre::HlmsUnlit *>(
        mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT));

    // The datablock belongs to the DEF (a def binds it once, in init()), so a
    // recycled def keeps the one it was created with and we mutate that.
    std::string dbName;
    auto dbIt = mParticleDatablocks.find(n.particleDef);
    Ogre::HlmsUnlitDatablock *db = nullptr;
    if (dbIt != mParticleDatablocks.end()) {
        dbName = dbIt->second;
        db = static_cast<Ogre::HlmsUnlitDatablock *>(
            hlmsUnlit->getDatablock(Ogre::IdString(dbName)));
    }
    if (!db) {
        dbName = processUniqueName("psys");
        db = static_cast<Ogre::HlmsUnlitDatablock *>(hlmsUnlit->createDatablock(
            Ogre::IdString(dbName), dbName, Ogre::HlmsMacroblock(), Ogre::HlmsBlendblock(),
            Ogre::HlmsParamVec()));
        db->setUseColour(true);
        db->setColour(Ogre::ColourValue::White);
    }

    // Depth test on, depth write off — particles never occlude each other and
    // must not write into the depth the rest of the frame reads.
    Ogre::HlmsMacroblock macro;
    macro.mDepthCheck = true; macro.mDepthWrite = false; macro.mCullMode = Ogre::CULL_NONE;
    Ogre::HlmsBlendblock blend;
    if (d.additive) {
        // (src-alpha, one). Order-independent by construction: additive fire,
        // embers and sparks need no sorting, and PFX2 never sorts.
        blend.mSourceBlendFactor = Ogre::SBF_SOURCE_ALPHA;
        blend.mDestBlendFactor   = Ogre::SBF_ONE;
    } else {
        blend.setBlendType(Ogre::SBT_TRANSPARENT_ALPHA);
        // Alpha blending IS order-dependent, and PFX2 does not sort. Alpha
        // hashing (stochastic transparency against blue noise) makes the draw
        // order stop mattering; it resolves cleanly only with MSAA + A2C, so
        // offscreen 1x views see the dither. That is expected, not a defect.
        blend.mAlphaToCoverage = d.alphaHash ? Ogre::HlmsBlendblock::A2cEnabledMsaaOnly
                                             : Ogre::HlmsBlendblock::A2cDisabled;
    }
    db->setMacroblock(macro);
    db->setBlendblock(blend);
    db->setAlphaHashing(!d.additive && d.alphaHash);
    Ogre::HlmsSamplerblock sampler;
    sampler.mU = Ogre::TAM_CLAMP; sampler.mV = Ogre::TAM_CLAMP;
    sampler.mMipFilter = Ogre::FO_LINEAR;
    db->setTexture(0, tex, tex ? &sampler : nullptr);
    return dbName;
}

bool OgreScene::buildParticleDef(Node &n, const ParticleSystemDesc &d,
                                 const ParticleTopology &topo) {
    const std::string key = topo.key();
    Ogre::ParticleSystemManager2 *mgr = mSceneMgr->getParticleSystemManager2();

    // Recycle first: a def with this exact frozen shape that some other node
    // abandoned. Its emitters and affectors are already the right kinds and in
    // the right order — only their VALUES need rewriting, which is what
    // applyParticleValues does for a fresh def too.
    auto poolIt = mParticleDefPool.find(key);
    if (poolIt != mParticleDefPool.end() && !poolIt->second.empty()) {
        n.particleDef = poolIt->second.back();
        poolIt->second.pop_back();
        n.particleTopology = key;
        return true;
    }

    Ogre::ParticleSystemDef *def = mgr->createParticleSystemDef(processUniqueName("psysdef"));
    ++mParticleDefsCreated;
    def->setParticleQuota(topo.quotaBucket);         // asserts !isInitialized(): before init only
    def->reserveNumEmitters(topo.emitterShapes.size());
    for (int shape : topo.emitterShapes)
        def->addEmitter(Ogre::IdString(emitterFactoryName(ParticleEmitterShape(shape))));
    def->reserveNumAffectors(topo.affectorKinds.size());
    for (int kind : topo.affectorKinds)
        def->addAffector(Ogre::IdString(affectorFactoryName(ParticleAffectorDesc::Kind(kind))));
    // AFTER addAffector: adding a Rotator flips rotation type to Texcoord (it
    // reports wantsRotation()), and we want Vertex — the quad's vertices spin,
    // not its UVs, which is what the legacy billboards did and what a flame
    // needs (a rotating UV set on a clamped texture tears the sprite).
    def->setRotationType(Ogre::ParticleRotationType::Vertex);
    n.particleDef = def;
    n.particleTopology = key;

    // The datablock must exist and be named before init(): init() looks
    // mMaterialName up in the HlmsManager and binds whatever it finds, once and
    // for all (setMaterialName afterwards is a no-op for a PFX2 def).
    const std::string dbName = ensureParticleDatablock(n, d);
    if (dbName.empty()) { n.particleDef = nullptr; return false; }
    mParticleDatablocks[def] = dbName;
    // Qualified: ParticleSystemDef inherits setMaterialName from BOTH
    // ParticleSystem (public) and Renderable (protected) — unqualified it is
    // ambiguous. ParticleSystem's is the one init() reads.
    def->Ogre::ParticleSystem::setMaterialName(
        dbName, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
    def->init(mRoot->getRenderSystem()->getVaoManager());
    return true;
}

void OgreScene::applyParticleValues(Node &n, const ParticleSystemDesc &d) {
    Ogre::ParticleSystemDef *def = n.particleDef;

    def->setParticleType(toOgreParticleType(d.orientation));
    def->setCommonVectors(toOgre(d.commonDirection), toOgre(d.commonUp));

    // ---- emitters ----
    const auto &emitters = def->getEmitters();
    for (size_t i = 0; i < emitters.size() && i < d.emitters.size(); ++i) {
        const ParticleEmitterDesc &e = d.emitters[i];
        Ogre::EmitterDefData *ed = emitters[i];
        // EmitterDefData derives PROTECTED from ParticleEmitter; asParticleEmitter()
        // is the sanctioned way back to the setters (and to StringInterface, which
        // is how the shape-specific extents are reached — the concrete emitter
        // classes live in the plugin and we do not link against it).
        Ogre::ParticleEmitter *pe = ed->asParticleEmitter();
        pe->setPosition(toOgre(e.position));
        pe->setDirection(toOgre(e.direction));
        pe->setAngle(Ogre::Degree(std::max(0.0f, e.angleDegrees)));
        pe->setEmissionRate(std::max(0.0f, e.rate));
        pe->setParticleVelocity(std::min(e.velocityMin, e.velocityMax),
                                std::max(e.velocityMin, e.velocityMax));
        pe->setTimeToLive(std::max(0.01f, std::min(e.ttlMin, e.ttlMax)),
                          std::max(0.01f, std::max(e.ttlMin, e.ttlMax)));
        pe->setColour(toOgre(e.colourStart), toOgre(e.colourEnd));
        pe->setDuration(std::max(0.0f, e.duration));
        pe->setRepeatDelay(std::max(0.0f, e.repeatDelay));
        pe->setStartTime(std::max(0.0f, e.startTime));
        ed->setInitialDimensions(Ogre::Vector2(std::max(0.0f, e.sizeWidth),
                                               std::max(0.0f, e.sizeHeight)));
        // Shape extents. Point ignores all of it. The area emitters (Box,
        // Cylinder, Ellipsoid, HollowEllipsoid) take width/height/depth; Ring
        // and HollowEllipsoid add the inner hole as a FRACTION of the outer.
        if (e.shape != ParticleEmitterShape::Point) {
            pe->setParameter("width",  realParam(std::max(0.0f, e.extents.x)));
            pe->setParameter("height", realParam(std::max(0.0f, e.extents.y)));
            pe->setParameter("depth",  realParam(std::max(0.0f, e.extents.z)));
        }
        if (e.shape == ParticleEmitterShape::Ring ||
            e.shape == ParticleEmitterShape::HollowEllipsoid) {
            pe->setParameter("inner_width",  realParam(std::min(0.999f, std::max(0.0f, e.innerExtents.x))));
            pe->setParameter("inner_height", realParam(std::min(0.999f, std::max(0.0f, e.innerExtents.y))));
        }
    }

    // ---- affectors ----
    // The affector list is positional: buildParticleDef added them in exactly
    // the order the desc lists them, and the topology key pins that order, so
    // index i here is desc affector i.
    const auto &affectors = def->getAffectors();
    // ScaleInterpolator REPLACES a particle's dimensions with (1,1) * scale —
    // it does not multiply the emitter's initial size (OgreScaleInterpolator
    // Affector2.cpp:76,92: `defaultDimensions = UNIT_SCALE`). So scale keys are
    // authored as MULTIPLIERS and folded against the first emitter's width
    // here; a system with scale-over-life therefore draws SQUARE particles.
    const float baseSize = d.emitters.empty() ? 1.0f : std::max(0.0f, d.emitters[0].sizeWidth);
    for (size_t i = 0; i < affectors.size() && i < d.affectors.size(); ++i) {
        const ParticleAffectorDesc &a = d.affectors[i];
        Ogre::ParticleAffector2 *af = affectors[i];
        switch (a.kind) {
        case ParticleAffectorDesc::Kind::ColourKeys: {
            float times[6]; Colour cols[6];
            fillInterpStages(a.colourKeyTimes, a.colourKeys, a.keyCount, times, cols);
            for (int s = 0; s < 6; ++s) {
                af->setParameter("colour" + std::to_string(s), colourParam(cols[s]));
                af->setParameter("time" + std::to_string(s), realParam(times[s]));
            }
            break;
        }
        case ParticleAffectorDesc::Kind::ScaleKeys: {
            float times[6]; float scales[6];
            fillInterpStages(a.scaleKeyTimes, a.scaleKeys, a.keyCount, times, scales);
            for (int s = 0; s < 6; ++s) {
                af->setParameter("scale" + std::to_string(s), realParam(scales[s] * baseSize));
                af->setParameter("time" + std::to_string(s), realParam(times[s]));
            }
            break;
        }
        case ParticleAffectorDesc::Kind::Rotator:
            af->setParameter("rotation_speed_range_start", realParam(a.rotSpeedMin));
            af->setParameter("rotation_speed_range_end",   realParam(a.rotSpeedMax));
            af->setParameter("rotation_range_start",       realParam(a.rotStart));
            af->setParameter("rotation_range_end",         realParam(a.rotEnd));
            break;
        case ParticleAffectorDesc::Kind::LinearForce:
            af->setParameter("force_vector", vec3Param(a.force));
            af->setParameter("force_application", a.forceAverage ? "average" : "add");
            break;
        case ParticleAffectorDesc::Kind::Turbulence:
            af->setParameter("randomness", realParam(std::max(0.0f, a.randomness)));
            af->setParameter("scope",      realParam(std::min(1.0f, std::max(0.0f, a.scope))));
            af->setParameter("keep_velocity", a.keepVelocity ? "true" : "false");
            break;
        case ParticleAffectorDesc::Kind::DeflectorPlane:
            af->setParameter("plane_point",  vec3Param(a.planePoint));
            af->setParameter("plane_normal", vec3Param(a.planeNormal));
            af->setParameter("bounce",       realParam(a.bounce));
            break;
        }
    }
}

bool OgreScene::setParticleSystem(NodeId id, const ParticleSystemDesc &d) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) { mError = "setParticleSystem: unknown node"; return false; }
    if (d.emitters.empty()) { mError = "setParticleSystem: no emitters"; return false; }
    if (!Ogre::ParticleSystemManager2::getFactory(Ogre::IdString("Point"))) {
        mError = "setParticleSystem: Plugin_ParticleFX2 is not loaded "
                 "(no emitter/affector factories are registered)";
        return false;
    }
    JAH_TRY {
        Node &n = it->second;

        ParticleTopology topo;
        topo.quotaBucket = quotaBucketFor(d.quota);
        for (const auto &e : d.emitters) topo.emitterShapes.push_back(int(e.shape));
        for (const auto &a : d.affectors) topo.affectorKinds.push_back(int(a.kind));

        const bool rebuild = !n.particleDef || n.particleTopology != topo.key();
        if (rebuild) {
            releaseParticleSystem(n);
            if (!buildParticleDef(n, d, topo)) return false;
        }
        // The datablock is per-def and mutated in place: a texture or blend-mode
        // change costs nothing and never rebuilds. (A recycled def already has
        // its datablock; ensureParticleDatablock finds and rewrites it.)
        const std::string dbName = ensureParticleDatablock(n, d);
        if (dbName.empty()) return false;
        mParticleDatablocks[n.particleDef] = dbName;

        applyParticleValues(n, d);

        if (!n.particleSystem) {
            n.particleSystem = mSceneMgr->createParticleSystem2(n.particleDef);
            // MANDATORY: emission reads system->getParentNode() with no null
            // check (OgreParticleSystemManager2.cpp:213-215). Attaching to the
            // node's own SceneNode is also what makes the emitter follow the
            // document node's position and orientation.
            n.node->attachObject(n.particleSystem);
        }
        // A recycled or rebuilt def arrives HIDDEN (releaseParticleSystem parks
        // it with visibility 0), so the node's own visibility has to be pushed
        // onto it here. Node::visible is the record setNodeVisible keeps for
        // exactly this: the def is not a child of the node in Ogre's graph — it
        // hangs off the STATIC root — so no visibility cascade ever reaches it.
        n.particleDef->setVisibilityFlags(n.visible ? 1u : 0u);
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::removeParticleSystem(NodeId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.particleDef) return false;
    JAH_TRY { releaseParticleSystem(it->second); return true; } JAH_CATCH(mError, false);
}

unsigned OgreScene::particleCount(NodeId id) const {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.particleDef) return 0;
    // SIMD-rounded and never above the quota: the backend tracks live particles
    // as a bitset between a first and a last index and exposes only the packed
    // span between them (getNumSimdActiveParticles). Good enough to answer "is
    // this system emitting", which is all any caller asks.
    return unsigned(it->second.particleDef->getNumSimdActiveParticles());
}

void OgreScene::releaseParticleSystem(Node &n) {
    if (!n.particleDef) return;
    if (n.particleSystem) {
        n.particleSystem->detachFromParent();
        mSceneMgr->destroyParticleSystem2(n.particleSystem);
        n.particleSystem = nullptr;
    }
    // The def survives — nothing can destroy one before the SceneManager dies.
    // Hide it (the def is what the render queue tests, so its live particles
    // stop drawing immediately, which is exactly the "particles outlive their
    // emitter" problem solved) and park it for the next system of this shape.
    n.particleDef->setVisibilityFlags(0u);
    mParticleDefPool[n.particleTopology].push_back(n.particleDef);
    n.particleDef = nullptr;
    n.particleTopology.clear();
}

}}}  // namespace jahshaka::engine::detail
