// HlmsAtom — see HlmsAtom.h. The shape is the stage-0 spike's
// (spikes/atom-stage0/src/HlmsAtomSpike.cpp), which is the shape of Ogre's own
// HlmsTerra (Samples/2.0/Tutorials/Tutorial_Terrain/src/Terra/Hlms/OgreHlmsTerra.cpp):
// the constructor names the type and reserves the slots, getDefaultPaths makes
// PBS's piece folders our LIBRARIES, notifyPropertiesMergedPreGenerationStep names
// the reserved registers, setupRootLayout widens the read-only range over them, and
// fillBuffersForV2 calls PBS's and then binds what is ours. Terra copies the whole of
// PBS's fillBuffersFor (300 lines); this does not have to.
#include "HlmsAtom.h"

#include <CommandBuffer/OgreCbShaderBuffer.h>
#include <CommandBuffer/OgreCbTexture.h>
#include <CommandBuffer/OgreCommandBuffer.h>
#include <OgreConstBufferPool.h>
#include <OgreHlmsJson.h>
#include <OgreCamera.h>
#include <OgreHlmsManager.h>
#include <OgreResourceTransition.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreLogManager.h>
#include <OgreLight.h>
#include <OgreForwardClustered.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <OgreRenderQueue.h>
#include <OgreRenderSystem.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreRootLayout.h>
#include <OgreCamera.h>
#include <OgreSceneManager.h>
#include <OgreStagingTexture.h>
#include <OgreTextureBox.h>
#include <OgreTextureGpuManager.h>
#include <OgreTextureGpu.h>
#include <Vao/OgreConstBufferPacked.h>
#include <Vao/OgreIndexBufferPacked.h>
#include <Vao/OgreReadOnlyBufferPacked.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreVertexArrayObject.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace jahshaka {
namespace engine {
namespace detail {

const char *const HlmsAtom::kTypeName = "Atom";

namespace {
struct FsVertex {
    float px = 0, py = 0, pz = 0;              // POSITION: clip-space xy
    float nx = 0, ny = 0, nz = 0;              // NORMAL
    float tx = 0, ty = 0, tz = 0, tw = 0;      // TANGENT4
    float u = 0, v = 0;                        // UV0
};
// Clip space, z = 0, w = 1. In a CLASSIFIED pass the vertex stage replaces z with the
// draw's class depth (the per-draw word's .w; ATOM-DECODE-CLASS-1); anywhere else the
// twin's macroblock tests no depth and the id image alone decides coverage.
const FsVertex kFullScreenTri[3] = {
    { -1.0f, -1.0f, 0.0f, 0, 0, 1, 1, 0, 0, 1, 0, 0 },
    { 3.0f, -1.0f, 0.0f, 0, 0, 1, 1, 0, 0, 1, 0, 0 },
    { -1.0f, 3.0f, 0.0f, 0, 0, 1, 1, 0, 0, 1, 0, 0 },
};

/// The full-screen triangle's geometry — ONE layout, shared by every decode draw
/// and by the bucket key's probe (the key is the permutation over exactly this
/// layout).
Ogre::VertexArrayObject *createFullScreenVao(Ogre::VaoManager *vaoManager) {
    auto *indices = reinterpret_cast<Ogre::uint16 *>(
        OGRE_MALLOC_SIMD(sizeof(Ogre::uint16) * 3u, Ogre::MEMCATEGORY_GEOMETRY));
    indices[0] = 0;
    indices[1] = 1;
    indices[2] = 2;
    Ogre::IndexBufferPacked *indexBuffer = vaoManager->createIndexBuffer(
        Ogre::IndexBufferPacked::IT_16BIT, 3u, Ogre::BT_IMMUTABLE, indices, true);
    Ogre::VertexElement2Vec elements;
    elements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
    elements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_NORMAL));
    elements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT4, Ogre::VES_TANGENT));
    elements.push_back(Ogre::VertexElement2(Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES));
    auto *verts = reinterpret_cast<FsVertex *>(
        OGRE_MALLOC_SIMD(sizeof(kFullScreenTri), Ogre::MEMCATEGORY_GEOMETRY));
    std::memcpy(verts, kFullScreenTri, sizeof(kFullScreenTri));
    Ogre::VertexBufferPacked *vertexBuffer =
        vaoManager->createVertexBuffer(elements, 3u, Ogre::BT_IMMUTABLE, verts, true);
    Ogre::VertexBufferPackedVec vertexBuffers;
    vertexBuffers.push_back(vertexBuffer);
    return vaoManager->createVertexArrayObject(vertexBuffers, indexBuffer, Ogre::OT_TRIANGLE_LIST);
}

void destroyFullScreenVao(Ogre::VaoManager *vaoManager, Ogre::VertexArrayObject *vao) {
    if (!vao || !vaoManager) return;
    Ogre::IndexBufferPacked *ib = vao->getIndexBuffer();
    const Ogre::VertexBufferPackedVec vbs = vao->getVertexBuffers();
    vaoManager->destroyVertexArrayObject(vao);
    if (ib) vaoManager->destroyIndexBuffer(ib);
    for (Ogre::VertexBufferPacked *vb : vbs) vaoManager->destroyVertexBuffer(vb);
}
}  // namespace

/// THE BUCKET KEY'S PROBE: a bare Renderable (no MovableObject, never queued,
/// never drawn) carrying the full-screen layout. It WEARS a datablock only for
/// the length of one calculateHashFor and never links to it (a linked renderable
/// would be flushed and re-hashed by PBS on every edit of that datablock).
class AtomKeyProbe final : public Ogre::Renderable {
public:
    explicit AtomKeyProbe(Ogre::VaoManager *vaoManager) : mVaoManager(vaoManager) {
        mVao = createFullScreenVao(vaoManager);
        mVaoPerLod[Ogre::VpNormal].push_back(mVao);
        mVaoPerLod[Ogre::VpShadow].push_back(mVao);
    }
    ~AtomKeyProbe() override {
        mHlmsDatablock = nullptr;   // never linked (see wear)
        mVaoPerLod[Ogre::VpNormal].clear();
        mVaoPerLod[Ogre::VpShadow].clear();
        destroyFullScreenVao(mVaoManager, mVao);
    }
    void wear(Ogre::HlmsDatablock *db) { mHlmsDatablock = db; }
    void getRenderOperation(Ogre::v1::RenderOperation &, bool) override {
        OGRE_EXCEPT(Ogre::Exception::ERR_NOT_IMPLEMENTED, "v2 only", "AtomKeyProbe");
    }
    void getWorldTransforms(Ogre::Matrix4 *) const override {
        OGRE_EXCEPT(Ogre::Exception::ERR_NOT_IMPLEMENTED, "v2 only", "AtomKeyProbe");
    }
    const Ogre::LightList &getLights() const override { return mNoLights; }
    bool getCastsShadows() const override { return false; }

private:
    Ogre::VaoManager *mVaoManager = nullptr;
    Ogre::VertexArrayObject *mVao = nullptr;
    Ogre::LightList mNoLights;
};


HlmsAtom::HlmsAtom(Ogre::Archive *dataFolder, Ogre::ArchiveVec *libraryFolders)
    : Ogre::HlmsPbs(dataFolder, libraryFolders) {
    // Terra's own three lines (OgreHlmsTerra.cpp:87-89).
    mType = kType;
    mTypeName = kTypeName;
    mTypeNameStr = kTypeName;
    // PBS reserves ONE tex-buffer slot, the vertex shader's worldMatBuf. The decode
    // needs seven more (the instance table, the level table, the geometry rows, the
    // bucket table, the hit list, the mesh table and the cluster table) and one
    // texture (the id image). PBS's own slot accounting honours
    // both counters (OgreHlmsPbs.cpp: the registers in notifyPropertiesMergedPre-
    // GenerationStep, the binds in fillBuffersFor), so nothing of PBS's moves onto them.
    mReservedTexBufferSlots = kReservedBufSlots;
    // The id image (hit mode's record image) — ONE texture: the pass textures
    // share the pin's 32-slot table (kHitBufSlot, HlmsAtom.h).
    mReservedTexSlots = 1u;
}

HlmsAtom::~HlmsAtom() {
    destroyDecodeTwins();
    delete mKeyProbe;
    mKeyProbe = nullptr;
    if (mPointSampler && mHlmsManager) mHlmsManager->destroySamplerblock(mPointSampler);
    mPointSampler = nullptr;
    if (mEmptyBuf && mVaoManager) mVaoManager->destroyReadOnlyBuffer(mEmptyBuf);
    mEmptyBuf = nullptr;
    if (mWorldLightBuf && mVaoManager) mVaoManager->destroyReadOnlyBuffer(mWorldLightBuf);
    mWorldLightBuf = nullptr;
    if (mEmptyIds && mRenderSystem && mRenderSystem->getTextureGpuManager())
        mRenderSystem->getTextureGpuManager()->destroyTexture(mEmptyIds);
    mEmptyIds = nullptr;
}

/// THE STAND-INS a draw without a source binds: a 1x1 id image holding "nothing
/// covers this pixel" and a zero table holding ONE WHOLE ENTRY of the largest table
/// (a GpuInstance, 160 bytes) — the decode clamps every index into its table and
/// reads unconditionally, so a stand-in must be a table it can read in range. Zeros
/// name no bucket (a twin's entry is its slot PLUS ONE), so the draw shades nothing.
/// Created on the first draw that needs them.
void HlmsAtom::ensureStandIns() {
    if (!mVaoManager || !mRenderSystem) return;
    if (!mEmptyBuf) {
        uint32_t zeros[40] = {};
        mEmptyBuf = mVaoManager->createReadOnlyBuffer(Ogre::PFG_R32_UINT, sizeof(zeros), Ogre::BT_IMMUTABLE,
                                                      zeros, false);
    }
    if (!mEmptyIds) {
        Ogre::TextureGpuManager *tm = mRenderSystem->getTextureGpuManager();
        // A ManualTexture goes Resident by an immediate transition and is never
        // notifyDataIsReady'd (DOCS/traps/ENGINE.md).
        mEmptyIds = tm->createTexture("jahAtomEmptyIds", Ogre::GpuPageOutStrategy::Discard,
                                      Ogre::TextureFlags::ManualTexture, Ogre::TextureTypes::Type2D);
        mEmptyIds->setResolution(1u, 1u);
        mEmptyIds->setPixelFormat(Ogre::PFG_RG32_UINT);
        mEmptyIds->setNumMipmaps(1u);
        mEmptyIds->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        Ogre::StagingTexture *st = tm->getStagingTexture(1u, 1u, 1u, 1u, Ogre::PFG_RG32_UINT);
        st->startMapRegion();
        Ogre::TextureBox box = st->mapRegion(1u, 1u, 1u, 1u, Ogre::PFG_RG32_UINT);
        auto *px = reinterpret_cast<uint32_t *>(box.at(0, 0, 0));
        px[0] = AtomId::kEmpty;
        px[1] = 0u;
        st->stopMapRegion();
        st->upload(box, mEmptyIds, 0, nullptr, nullptr, true);
        tm->removeStagingTexture(st);
    }
}

void HlmsAtom::getDefaultPaths(Ogre::String &outDataFolderPath,
                               Ogre::StringVector &outLibraryFoldersPaths) {
    // PBS's list FIRST, verbatim, so the library order — and therefore which piece
    // wins a redefinition — is PBS's; then the engine's own pieces, LAST among the
    // lighting libraries exactly as registerHlms gives them to PBS (the fog piece
    // redefines one of Pbs/Any/Atmosphere and a redefinition only works after the
    // original was collected); then ours.
    Ogre::String pbsData;
    Ogre::HlmsPbs::getDefaultPaths(pbsData, outLibraryFoldersPaths);
    // ...AND PBS's DATA FOLDER'S PIECE FILES (Hlms/Pbs/<syntax>/*_piece_*): the
    // data folder is ours, so a piece PBS keeps beside its templates —
    // `DeclDecalsSamplers` (Forward3D_piece_ps.glsl), the decal textures'
    // declarations — would be missing here, and a hit that finds a Forward+ cell
    // with a decal (PHOTON-HIT-SHADE-1) samples undeclared textures. A library
    // folder loads piece files only, never the templates beside them.
    outLibraryFoldersPaths.push_back(pbsData);
    outLibraryFoldersPaths.push_back("Hlms/Jahshaka");
    outLibraryFoldersPaths.push_back("Hlms/Atom/Any");
    // PBS's data folder is Hlms/Pbs/<syntax>; ours is its sibling.
    const Ogre::String::size_type slash = pbsData.find_last_of('/');
    const Ogre::String syntax =
        slash == Ogre::String::npos ? Ogre::String("GLSL") : pbsData.substr(slash + 1u);
    outDataFolderPath = "Hlms/Atom/" + syntax;
}

void HlmsAtom::setDecodeSource(const DecodeSource &src) {
    mSource = src;
    if (!mPointSampler && mHlmsManager) {
        // ACQUIRED ONCE PER MANAGER (GATHER-0's trap: getSamplerblock takes a
        // reference and HlmsSamplerblock::mRefCount is a uint16).
        Ogre::HlmsSamplerblock ref;
        ref.setFiltering(Ogre::TFO_NONE);
        ref.setAddressingMode(Ogre::TAM_CLAMP);
        mPointSampler = mHlmsManager->getSamplerblock(ref);
    }
}

uint64_t HlmsAtom::textureSetKeyOf(const Ogre::HlmsDatablock *db) {
    if (!db || !db->getCreator() || db->getCreator()->getType() != Ogre::HLMS_PBS) return 0u;
    const auto *pbsDb = static_cast<const Ogre::HlmsPbsDatablock *>(db);
    // FNV-1a over the pointers, slot by slot (a swap between two slots moves it).
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const void *p) {
        uint64_t v = uint64_t(reinterpret_cast<uintptr_t>(p));
        for (int b = 0; b < 8; ++b) {
            h ^= (v & 0xFFu);
            h *= 1099511628211ull;
            v >>= 8u;
        }
    };
    for (Ogre::uint8 t = 0; t < Ogre::NUM_PBSM_TEXTURE_TYPES; ++t) {
        mix(pbsDb->getTexture(t));
        mix(pbsDb->getSamplerblock(t));
    }
    return h;
}

uint32_t HlmsAtom::materialWordOf(const Ogre::HlmsDatablock *db) {
    if (!db || !db->getCreator() || db->getCreator()->getType() != Ogre::HLMS_PBS)
        return kNoMaterialWord;
    const auto *pbsDb = static_cast<const Ogre::HlmsPbsDatablock *>(db);
    auto *pbs = static_cast<Ogre::HlmsPbs *>(db->getCreator());
    if (!pbsDb->getAssignedPool()) return kNoMaterialWord;
    // ConstBufferPool::getPoolIndex is public and names the pool "unique per hash";
    // every PBS datablock asks for hash 0 (HlmsPbsDatablock::createBuffers), so the
    // index is unique across the whole PBS instance.
    const size_t pool =
        pbs->getPoolIndex(const_cast<Ogre::HlmsPbsDatablock *>(pbsDb));
    return (uint32_t(pool) << 16u) | (pbsDb->getAssignedSlot() & 0xFFFFu);
}

// ---------------------------------------------------------------------------
// THE DECODE BUCKET (S3-DRAW)
// ---------------------------------------------------------------------------
namespace {
/// A property that does not reach the twin: the twin carries its own opaque
/// blendblock and its own depth-less CULL_NONE macroblock (decodeTwinForBucket), so the
/// PBS datablock's are not part of the permutation a decode draw compiles.
bool twinDiscards(const Ogre::IdString &key) {
    return key == Ogre::HlmsPsoProp::Macroblock || key == Ogre::HlmsPsoProp::Blendblock ||
           key == Ogre::HlmsBaseProp::AlphaBlend || key == Ogre::HlmsBaseProp::AlphaToCoverage;
}
void fnvMix(uint64_t &h, uint64_t v) {
    for (int b = 0; b < 8; ++b) {
        h ^= (v & 0xFFu);
        h *= 1099511628211ull;
        v >>= 8u;
    }
}
}  // namespace

bool HlmsAtom::bucketKeyOf(const Ogre::HlmsPbsDatablock *pbs, BucketKey &out, std::string &err) {
    if (!pbs || !pbs->getCreator() || pbs->getCreator()->getType() != Ogre::HLMS_PBS) {
        err = "bucketKeyOf: not an HlmsPbs datablock";
        return false;
    }
    for (size_t s = 0; s < Ogre::CustomPieceStage::NumCustomPieceStages; ++s) {
        if (pbs->getCustomPieceFileIdHash(Ogre::CustomPieceStage::CustomPieceStage(s))) {
            err = "bucketKeyOf: a per-datablock custom piece (the twin cannot carry it)";
            return false;
        }
    }
    const uint32_t word = materialWordOf(pbs);
    if (word == kNoMaterialWord) {
        err = "bucketKeyOf: the datablock has no const-buffer slot";
        return false;
    }
    // TEXTURES STILL BAKING: PBS itself delays the hash then (HlmsPbs::
    // calculateHashFor answers 0 while the descriptor sets are dirty), so there is
    // no permutation to key on yet — the datablock is a bucket OF ITS OWN until they
    // are baked (its twin, a clone, carries its textures exactly), and the witnesses
    // move it to its real bucket the frame its hash lands (forgetDecodeTwinIfMoved:
    // the key moved). The screen split routes a pending item to PBS meanwhile
    // (atomRouteFor); a RAY HIT on it is shaded through this singleton twin — a swap
    // of a live material's texture reaches its hits on the first frame.
    if (isBucketPending(pbs)) {
        out.permutation = 0x8000000000000000ull | uint64_t(reinterpret_cast<uintptr_t>(pbs));
        out.textures = textureSetKeyOf(pbs);
        out.pool = word >> 16u;
        return true;
    }
    if (!mKeyProbe) {
        if (!mVaoManager) {
            err = "bucketKeyOf: no VaoManager (the headless boot)";
            return false;
        }
        mKeyProbe = new AtomKeyProbe(mVaoManager);
    }
    // OGRE'S OWN DERIVATION: calculateHashFor over the full-screen layout wearing
    // the PBS datablock — the property set the twin (its JSON clone) generates,
    // up to the blend/macro state the twin replaces. The entry it leaves in this
    // Hlms's renderable cache is the one a twin of this bucket would find anyway.
    mKeyProbe->wear(const_cast<Ogre::HlmsPbsDatablock *>(pbs));
    Ogre::uint32 hash = 0u, casterHash = 0u;
    try { calculateHashFor(mKeyProbe, hash, casterHash); }
    catch (Ogre::Exception &e) {
        mKeyProbe->wear(nullptr);
        err = "bucketKeyOf: " + e.getFullDescription();
        return false;
    }
    mKeyProbe->wear(nullptr);
    const size_t idx = (hash >> Ogre::HlmsBits::RenderableShift) & Ogre::HlmsBits::RenderableMask;
    if (idx >= mRenderableCache.size()) {
        err = "bucketKeyOf: the renderable cache has no entry for the hash";
        return false;
    }
    const RenderableCache &rc = mRenderableCache[idx];
    uint64_t h = 1469598103934665603ull;
    for (const Ogre::HlmsProperty &p : rc.setProperties) {
        if (twinDiscards(p.keyName)) continue;
        fnvMix(h, p.keyName.getU32Value());
        fnvMix(h, uint64_t(uint32_t(p.value)));
    }
    for (size_t st = 0; st < Ogre::NumShaderTypes; ++st) {
        fnvMix(h, 0xA70Du + st);
        for (const auto &kv : rc.pieces[st]) {
            fnvMix(h, kv.first.getU32Value());
            for (const char c : kv.second) fnvMix(h, uint64_t(uint8_t(c)));
        }
    }
    out.permutation = h;
    out.textures = textureSetKeyOf(pbs);
    out.pool = word >> 16u;
    return true;
}

// ---------------------------------------------------------------------------
// THE DECODE TWIN
// ---------------------------------------------------------------------------
Ogre::HlmsPbsDatablock *HlmsAtom::decodeTwinForBucket(Ogre::HlmsPbsDatablock *pbs, std::string &err) {
    if (!pbs || !pbs->getCreator() || pbs->getCreator()->getType() != Ogre::HLMS_PBS) {
        err = "decodeTwinForBucket: not an HlmsPbs datablock";
        return nullptr;
    }
    if (auto it = mTwinOfPbs.find(pbs); it != mTwinOfPbs.end()) return it->second;
    BucketKey key;
    if (!bucketKeyOf(pbs, key, err)) return nullptr;
    const uint32_t word = materialWordOf(pbs);
    // A MEMBER JOINS ITS BUCKET: the twin already exists, and only the bucket
    // table learns the new word.
    if (auto kt = mTwinOfKey.find(key); kt != mTwinOfKey.end()) {
        Twin &t = mTwins[kt->second];
        t.members.emplace_back(pbs, word);
        mTwinOfPbs[pbs] = kt->second;
        mBucketDirty = true;
        ++mBucketGeneration;
        return kt->second;
    }

    // OGRE'S OWN SERIALISER IS THE COPY. HlmsJson writes one datablock under its
    // Hlms's type name; the same text under OUR type name loads through the loader
    // PBS's JSON half registers (HlmsAtom inherits HlmsPbs::_loadJson), into this
    // Hlms. Every permutation-relevant field travels — the textures (retrieved by
    // name, never reloaded), their samplers and uv sets, workflow, BRDF, transparency,
    // the maps — without a field list of ours to fall out of date. The bucket's
    // FIRST member is cloned; every later member has the same key, i.e. the same
    // permutation and textures, and differs only in the constants the decode reads
    // by slot from the pool.
    const Ogre::String *pbsName = pbs->getNameStr();
    if (!pbsName) {
        err = "decodeTwinForBucket: the datablock has no name to serialise";
        return nullptr;
    }
    // A BUCKET NEEDS A CLASS (the material depth's value); kMaxBuckets live buckets
    // at most — refused, never shared.
    const uint32_t classIdx = takeClass();
    if (!classIdx) {
        err = "decodeTwinForBucket: every class is in use (kMaxBuckets live buckets)";
        return nullptr;
    }
    const Ogre::String twinName = "jahAtomTwin/" + std::to_string(++mTwinSerial) + "/" + *pbsName;
    Ogre::String json;
    Ogre::HlmsJson hj(mHlmsManager, nullptr);
    hj.saveMaterial(pbs, json, "");
    const Ogre::String typeKey = "\"" + pbs->getCreator()->getTypeNameStr() + "\" : ";
    const Ogre::String::size_type at = json.find(typeKey);
    const Ogre::String nameKey = "\"" + *pbsName + "\" :";
    const Ogre::String::size_type nameAt =
        at == Ogre::String::npos ? Ogre::String::npos : json.find(nameKey, at + typeKey.size());
    if (at == Ogre::String::npos || nameAt == Ogre::String::npos) {
        err = "decodeTwinForBucket: unexpected JSON shape from HlmsJson::saveMaterial";
        return nullptr;
    }
    json.replace(nameAt, nameKey.size(), "\"" + twinName + "\" :");
    json.replace(at, typeKey.size(), "\"" + Ogre::String(kTypeName) + "\" : ");
    try {
        hj.loadMaterials("jahAtomTwin", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                         json.c_str(), "");
    } catch (Ogre::Exception &e) {
        releaseClass(classIdx);
        err = "decodeTwinForBucket: " + e.getFullDescription();
        return nullptr;
    }
    auto *twin = static_cast<Ogre::HlmsPbsDatablock *>(getDatablock(Ogre::IdString(twinName)));
    if (!twin) {
        releaseClass(classIdx);
        err = "decodeTwinForBucket: the twin did not load";
        return nullptr;
    }
    // THE DECODE'S MACROBLOCK: a full-screen triangle neither tests nor writes the
    // depth the id pass owns, and its winding is not the scene's (CULL_NONE — with
    // Ogre's default the decode draws NOTHING and a mean test reads it as "most
    // pixels differ", FINDINGS §2.4 (3)). A CLASSIFIED pass turns the test on,
    // EQUAL against the material depth, through the pass's strong-macroblock rule
    // (applyStrongMacroblockRules) — the twin itself stays depth-less for the hit
    // decode, whose pass has no material depth.
    Ogre::HlmsMacroblock macro;
    macro.mDepthCheck = false;
    macro.mDepthWrite = false;
    macro.mCullMode = Ogre::CULL_NONE;
    twin->setMacroblock(macro);
    // ...and NO BLENDING: a decode draw writes one record's (or one pixel's)
    // radiance, never a composite over what is under it — a transparent PBS
    // material's twin shades its surface as the hit decode's write-back expects
    // (PHOTON-HIT-SHADE-1; the day-one front end is opaque, D1 section 1).
    twin->setBlendblock(Ogre::HlmsBlendblock());
    // The decode never casts: its shadow-caster permutation is never requested.

    Twin t;
    t.pbs = pbs;
    t.twin = twin;
    t.key = key;
    t.bucketId = ++mBucketSerial;
    t.classIdx = classIdx;
    t.members.emplace_back(pbs, word);
    mTwins[twin] = t;
    mTwinOfPbs[pbs] = twin;
    mTwinOfKey[key] = twin;
    mBucketDirty = true;
    ++mBucketGeneration;
    return twin;
}

uint32_t HlmsAtom::bucketIdOf(const Ogre::HlmsDatablock *pbs) const {
    auto it = mTwinOfPbs.find(pbs);
    if (it == mTwinOfPbs.end()) return 0u;
    auto t = mTwins.find(static_cast<const Ogre::HlmsDatablock *>(it->second));
    return t == mTwins.end() ? 0u : t->second.bucketId;
}

uint32_t HlmsAtom::classOf(const Ogre::HlmsDatablock *pbs) const {
    auto it = mTwinOfPbs.find(pbs);
    if (it == mTwinOfPbs.end()) return 0u;
    auto t = mTwins.find(static_cast<const Ogre::HlmsDatablock *>(it->second));
    return t == mTwins.end() ? 0u : t->second.classIdx;
}

uint32_t HlmsAtom::takeClass() {
    // THE LOWEST FREE CLASS: the material depth's values stay small and dense.
    if (mClassUsed.empty()) mClassUsed.push_back(true);   // class 0 = "no bucket"
    for (uint32_t c = 1u; c < uint32_t(mClassUsed.size()); ++c)
        if (!mClassUsed[c]) {
            mClassUsed[c] = true;
            return c;
        }
    if (mClassUsed.size() > size_t(kMaxBuckets)) return 0u;
    mClassUsed.push_back(true);
    return uint32_t(mClassUsed.size() - 1u);
}

void HlmsAtom::releaseClass(uint32_t c) {
    if (c && c < mClassUsed.size()) mClassUsed[c] = false;
}

Ogre::HlmsPbsDatablock *HlmsAtom::classifyDatablock() {
    if (mClassifyDb) return mClassifyDb;
    // Every pixel of the pass's viewport is written (a pixel no id covers gets 0,
    // which no class equals), whatever the attachment held; no colour is.
    Ogre::HlmsMacroblock macro;
    macro.mDepthCheck = true;
    macro.mDepthWrite = true;
    macro.mDepthFunc = Ogre::CMPF_ALWAYS_PASS;
    macro.mCullMode = Ogre::CULL_NONE;
    Ogre::HlmsBlendblock blend;
    blend.mBlendChannelMask = 0u;
    mClassifyDb = static_cast<Ogre::HlmsPbsDatablock *>(
        createDatablock(Ogre::IdString("jahAtomClassify"), "jahAtomClassify", macro, blend,
                        Ogre::HlmsParamVec(), false));
    return mClassifyDb;
}

void HlmsAtom::forgetDecodeTwinOf(const Ogre::HlmsDatablock *pbs) {
    auto it = mTwinOfPbs.find(pbs);
    if (it == mTwinOfPbs.end()) return;
    Ogre::HlmsPbsDatablock *twin = it->second;
    mTwinOfPbs.erase(it);
    mBucketDirty = true;
    ++mBucketGeneration;
    // The epoch moves on every departure: a scene's synced word set may now name
    // a datablock that no longer exists, or one that belongs to another bucket.
    ++mTwinEpoch;
    auto tt = mTwins.find(twin);
    if (tt == mTwins.end()) return;
    Twin &t = tt->second;
    t.members.erase(std::remove_if(t.members.begin(), t.members.end(),
                                   [pbs](const std::pair<const Ogre::HlmsDatablock *, uint32_t> &m) {
                                       return m.first == pbs;
                                   }),
                    t.members.end());
    if (!t.members.empty()) {
        // THE BUCKET LIVES ON: the twin is a clone of the bucket's state, which
        // every remaining member shares (the key), and the pool the draw binds is
        // any member's (the key holds it) — never the departed one's pointer.
        if (t.pbs == pbs)
            t.pbs = static_cast<Ogre::HlmsPbsDatablock *>(
                const_cast<Ogre::HlmsDatablock *>(t.members.front().first));
        return;
    }
    // THE LAST MEMBER LEFT: the product's draws of the twin die first, in every
    // scene (Ogre asserts on a datablock with linked renderables), then the twin.
    for (auto &kv : mSceneDecodes) destroySceneDraw(kv.second, twin);
    mTwinOfKey.erase(t.key);
    releaseClass(t.classIdx);
    mTwins.erase(tt);
    // A decode draw may still carry the twin; the owner detaches its renderables
    // before it destroys materials (the grid's arm does), and Ogre asserts on a
    // datablock with linked renderables.
    if (twin && twin->getNameStr()) destroyDatablock(twin->getName());
}

bool HlmsAtom::decodeTwinMoved(const Ogre::HlmsDatablock *pbs, uint32_t &word) {
    auto it = mTwinOfPbs.find(pbs);
    if (it == mTwinOfPbs.end()) return false;
    word = materialWordOf(pbs);
    auto tt = mTwins.find(it->second);
    BucketKey key;
    std::string err;
    if (tt == mTwins.end() || !pbs->getCreator() || pbs->getCreator()->getType() != Ogre::HLMS_PBS ||
        !bucketKeyOf(static_cast<const Ogre::HlmsPbsDatablock *>(pbs), key, err))
        return true;
    if (key == tt->second.key) return false;
    // A SINGLETON TWIN MADE WHILE THE DATABLOCK'S TEXTURES BAKED (bucketKeyOf's
    // pending key, which names this datablock) is a clone of it as it now is: the
    // bake that followed re-hashed the datablock and gave it its real bucket, but the
    // same textures and pool are the same draw. It moves to that bucket at the next
    // drain (forgetDecodeTwinIfMoved); it is not STALE for this frame's decode — a
    // same-slot texture swap reaches its hits on the first frame (gi.hit_shade (g)).
    const uint64_t pendingKey = 0x8000000000000000ull | uint64_t(reinterpret_cast<uintptr_t>(pbs));
    return !(tt->second.key.permutation == pendingKey && key.textures == tt->second.key.textures &&
             key.pool == tt->second.key.pool);
}

void HlmsAtom::forgetMovedDecodeTwins() {
    std::vector<const Ogre::HlmsDatablock *> served;
    served.reserve(mTwinOfPbs.size());
    for (const auto &kv : mTwinOfPbs) served.push_back(kv.first);
    for (const Ogre::HlmsDatablock *db : served) forgetDecodeTwinIfMoved(db);
}

bool HlmsAtom::forgetDecodeTwinIfMoved(const Ogre::HlmsDatablock *pbs) {
    auto it = mTwinOfPbs.find(pbs);
    if (it == mTwinOfPbs.end()) return false;
    auto tt = mTwins.find(it->second);
    BucketKey key;
    std::string err;
    if (tt != mTwins.end() && pbs->getCreator() && pbs->getCreator()->getType() == Ogre::HLMS_PBS &&
        bucketKeyOf(static_cast<const Ogre::HlmsPbsDatablock *>(pbs), key, err) && key == tt->second.key)
        return false;
    forgetDecodeTwinOf(pbs);
    return true;
}

void forgetDecodeTwinOf(const Ogre::HlmsDatablock *pbs) {
    if (!pbs || !pbs->getCreator() || pbs->getCreator()->getType() != Ogre::HLMS_PBS) return;
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    if (auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr)
        atom->forgetDecodeTwinOf(pbs);
}

void forgetSceneDecodes(Ogre::SceneManager *sm) {
    if (!sm) return;
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    if (auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr)
        atom->forgetSceneManager(sm);
}

void HlmsAtom::destroySceneDraw(SceneDecodes &sd, const Ogre::HlmsDatablock *twin) {
    for (SceneDecodes::Set *set : { &sd.hit, &sd.screen }) {
        auto it = set->draws.find(twin);
        if (it == set->draws.end()) continue;
        if (sd.node && it->second->getParentSceneNode()) sd.node->detachObject(it->second);
        delete it->second;
        set->draws.erase(it);
    }
}

void HlmsAtom::syncDraws(Ogre::SceneManager *sm, SceneDecodes &sd, SceneDecodes::Set &set,
                         const std::vector<uint32_t> &words, Ogre::uint8 renderQueue) {
    // WORD -> PBS DATABLOCK, from PBS's own map (a word is {pool | slot}; a dead
    // datablock's word may be a new datablock's now — twinEpoch says so).
    std::unordered_map<uint32_t, Ogre::HlmsPbsDatablock *> byWord;
    {
        Ogre::Hlms *pbs = mHlmsManager->getHlms(Ogre::HLMS_PBS);
        if (!pbs) return;
        for (const auto &kv : pbs->getDatablockMap()) {
            const uint32_t w = materialWordOf(kv.second.datablock);
            if (w != kNoMaterialWord)
                byWord[w] = static_cast<Ogre::HlmsPbsDatablock *>(kv.second.datablock);
        }
    }
    std::unordered_map<const Ogre::HlmsDatablock *, bool> wanted;
    for (const uint32_t w : words) {
        auto it = byWord.find(w);
        if (it == byWord.end()) continue;
        // A REFRACTIVE material stays on stock HlmsPbs (D1 section 1's front-end
        // list): its pieces read the refraction pass' screen copy, which no decode
        // pass has. A hit on it is not shaded by the decode (stated, not hidden).
        if (it->second->getTransparencyMode() == Ogre::HlmsPbsDatablock::Refractive) continue;
        std::string err;
        Ogre::HlmsPbsDatablock *twin = decodeTwinForBucket(it->second, err);
        if (!twin) {
            // Once per datablock name: a material the decode cannot serve (a
            // per-datablock custom piece) keeps its hits unshaded — stated.
            static std::unordered_map<std::string, bool> sSaid;
            const Ogre::String *name = it->second->getNameStr();
            const std::string key = name ? *name : std::string("?");
            if (!sSaid[key]) {
                sSaid[key] = true;
                Ogre::LogManager::getSingleton().logMessage(
                    "HlmsAtom: no decode twin for '" + key + "' (" + err + ")");
            }
            continue;
        }
        wanted[twin] = true;
        if (set.draws.count(twin)) continue;
        if (!sd.node) sd.node = sm->getRootSceneNode()->createChildSceneNode(Ogre::SCENE_DYNAMIC);
        auto *d = new AtomDecodeRenderable(Ogre::Id::generateNewId<Ogre::MovableObject>(),
                                           &sm->_getEntityMemoryManager(Ogre::SCENE_DYNAMIC), sm,
                                           renderQueue);
        d->setDatablock(twin);
        sd.node->attachObject(d);
        // Hidden AFTER the attach (the parity suite's measured order).
        d->setVisible(set.shown);
        set.draws[twin] = d;
    }
    std::vector<const Ogre::HlmsDatablock *> gone;
    for (const auto &kv : set.draws)
        if (!wanted.count(kv.first)) gone.push_back(kv.first);
    for (const Ogre::HlmsDatablock *t : gone) {
        auto it = set.draws.find(t);
        if (sd.node && it->second->getParentSceneNode()) sd.node->detachObject(it->second);
        delete it->second;
        set.draws.erase(it);
    }
}

bool HlmsAtom::isBucketPending(const Ogre::HlmsDatablock *pbs) {
    const auto *user = pbs ? dynamic_cast<const Ogre::ConstBufferPoolUser *>(pbs) : nullptr;
    return user && (user->getDirtyFlags() &
                    (Ogre::ConstBufferPool::DirtyTextures | Ogre::ConstBufferPool::DirtySamplers));
}

void HlmsAtom::syncSceneDecodes(Ogre::SceneManager *sm, const std::vector<uint32_t> &words) {
    if (!sm || !mHlmsManager) return;
    SceneDecodes &sd = mSceneDecodes[sm];
    syncDraws(sm, sd, sd.hit, words, kHitDecodeRenderQueue);
}

void HlmsAtom::syncScreenDecodes(Ogre::SceneManager *sm, const std::vector<uint32_t> &words) {
    if (!sm || !mHlmsManager) return;
    SceneDecodes &sd = mSceneDecodes[sm];
    syncDraws(sm, sd, sd.screen, words, kScreenDecodeRenderQueue);
    // THE CLASSIFIER lives while a bucket draw does (ATOM-DECODE-CLASS-1).
    if (!sd.screen.draws.empty() && !sd.classify) {
        if (!sd.node) sd.node = sm->getRootSceneNode()->createChildSceneNode(Ogre::SCENE_DYNAMIC);
        sd.classify = new AtomDecodeRenderable(Ogre::Id::generateNewId<Ogre::MovableObject>(),
                                               &sm->_getEntityMemoryManager(Ogre::SCENE_DYNAMIC), sm,
                                               kScreenClassifyRenderQueue);
        sd.classify->setDatablock(classifyDatablock());
        sd.node->attachObject(sd.classify);
        sd.classify->setVisible(sd.screen.shown && mSource.classified);
    } else if (sd.screen.draws.empty() && sd.classify) {
        destroyClassifier(sd);
    }
}

void HlmsAtom::destroyClassifier(SceneDecodes &sd) {
    if (!sd.classify) return;
    if (sd.node && sd.classify->getParentSceneNode()) sd.node->detachObject(sd.classify);
    delete sd.classify;
    sd.classify = nullptr;
}

void HlmsAtom::showSceneDecodes(Ogre::SceneManager *sm, bool on) {
    auto it = mSceneDecodes.find(sm);
    if (it == mSceneDecodes.end()) return;
    it->second.hit.shown = on;
    for (auto &kv : it->second.hit.draws) kv.second->setVisible(on);
}

void HlmsAtom::armForWarmUp(Ogre::SceneManager *sm, bool on) {
    if (!sm) return;
    if (on) {
        ensureStandIns();
        DecodeSource src;
        src.ids = mEmptyIds;
        // The product's screen decode is always classified: the permutations the
        // warm-up compiles are the classified ones.
        src.classified = true;
        setDecodeSource(src);
    } else {
        setDecodeSource(DecodeSource());
    }
    SceneDecodes &sd = mSceneDecodes[sm];
    sd.screen.shown = on;
    for (auto &kv : sd.screen.draws) kv.second->setVisible(on);
    if (sd.classify) sd.classify->setVisible(on);
}

void HlmsAtom::showScreenDecodes(Ogre::SceneManager *sm, bool on) {
    auto it = mSceneDecodes.find(sm);
    if (it == mSceneDecodes.end()) return;
    it->second.screen.shown = on;
    for (auto &kv : it->second.screen.draws) kv.second->setVisible(on);
    // The classifier draws only in a CLASSIFIED pass (the source set just before).
    if (it->second.classify) it->second.classify->setVisible(on && mSource.classified);
}

size_t HlmsAtom::sceneDecodeCount(const Ogre::SceneManager *sm) const {
    auto it = mSceneDecodes.find(const_cast<Ogre::SceneManager *>(sm));
    return it == mSceneDecodes.end() ? 0u : it->second.hit.draws.size();
}

size_t HlmsAtom::screenDecodeCount(const Ogre::SceneManager *sm) const {
    auto it = mSceneDecodes.find(const_cast<Ogre::SceneManager *>(sm));
    return it == mSceneDecodes.end() ? 0u : it->second.screen.draws.size();
}

void HlmsAtom::forgetSceneManager(Ogre::SceneManager *sm) {
    mWorldLightScenes.erase(sm);
    auto it = mSceneDecodes.find(sm);
    if (it == mSceneDecodes.end()) return;
    std::vector<const Ogre::HlmsDatablock *> all;
    for (const auto &kv : it->second.hit.draws) all.push_back(kv.first);
    for (const auto &kv : it->second.screen.draws) all.push_back(kv.first);
    for (const Ogre::HlmsDatablock *t : all) destroySceneDraw(it->second, t);
    destroyClassifier(it->second);
    if (it->second.node) sm->destroySceneNode(it->second.node);
    mSceneDecodes.erase(it);
}

void HlmsAtom::destroyDecodeTwins() {
    // The product's draws first (they wear the twins), every scene.
    for (auto &kv : mSceneDecodes) {
        std::vector<const Ogre::HlmsDatablock *> all;
        for (const auto &d : kv.second.hit.draws) all.push_back(d.first);
        for (const auto &d : kv.second.screen.draws) all.push_back(d.first);
        for (const Ogre::HlmsDatablock *t : all) destroySceneDraw(kv.second, t);
        destroyClassifier(kv.second);
    }
    ++mTwinEpoch;
    for (auto &kv : mTwins) {
        if (kv.second.twin && kv.second.twin->getNameStr())
            destroyDatablock(kv.second.twin->getName());
    }
    mTwins.clear();
    mTwinOfPbs.clear();
    mTwinOfKey.clear();
    mClassUsed.clear();
    // The classifier's datablock (its draws died above, in every scene).
    if (mClassifyDb) destroyDatablock(Ogre::IdString("jahAtomClassify"));
    mClassifyDb = nullptr;
    if (mBucketBuf && mVaoManager) mVaoManager->destroyReadOnlyBuffer(mBucketBuf);
    mBucketBuf = nullptr;
    mBucketMirror.clear();
    mBucketDirty = true;
    ++mBucketGeneration;
}

/// THE BUCKET TABLE: one uint per PBS (pool, slot) — the CLASS of that material's
/// bucket (Twin::classIdx), 0 where no bucket serves it.
/// Rewritten only when a bucket gains or loses a member.
void HlmsAtom::uploadBucketTable() {
    if (!mBucketDirty || !mVaoManager) return;
    mBucketDirty = false;
    uint32_t pools = 1u;
    for (const auto &kv : mTwins) pools = std::max(pools, kv.second.key.pool + 1u);
    const uint32_t perPool = mSlotsPerPool;
    std::vector<uint32_t> table(size_t(pools) * perPool, 0u);
    for (const auto &kv : mTwins) {
        // EVERY MEMBER of the bucket names the bucket's one class.
        const uint32_t entry = kv.second.classIdx;
        for (const auto &m : kv.second.members) {
            const uint32_t pool = m.second >> 16u, slot = m.second & 0xFFFFu;
            if (pool < pools && slot < perPool) table[size_t(pool) * perPool + slot] = entry;
        }
    }
    // A read-only buffer's ELEMENT is one byte (VaoManager::createReadOnlyBuffer
    // hands the Vulkan implementation bytesPerElement = 1), so sizes and upload
    // ranges below are in bytes.
    const size_t bytes = table.size() * sizeof(uint32_t);
    if (mBucketBuf && mBucketBuf->getNumElements() < bytes) {
        mVaoManager->destroyReadOnlyBuffer(mBucketBuf);
        mBucketBuf = nullptr;
    }
    if (!mBucketBuf)
        mBucketBuf = mVaoManager->createReadOnlyBuffer(Ogre::PFG_R32_UINT, bytes, Ogre::BT_DEFAULT,
                                                       nullptr, false);
    mBucketBuf->upload(table.data(), 0u, bytes);
    mBucketMirror.swap(table);
}

/// THE WORLD LIGHT LIST (D3-HIT-SHADE-2). A ray hit outside the camera's frustum
/// has no Forward+ cell (the fork's fwdCustomFragCoord hook), and Ogre's clustered
/// list holds only the lights whose range touches that frustum
/// (ForwardClustered::collectLights culls against the camera; even the scene's
/// "global" list is culled against every camera, SceneManager::buildLightList). The
/// physics does not care where the camera is: a lamp lights every point inside its
/// range. So the hit decode carries its own list: EVERY point and spot light of the
/// scene the pass buffer does not already hold — the shadow node's casting lights
/// ride the pass buffer, exactly the set ForwardClustered excludes — in WORLD space
/// (the shader moves it through the pass's view), Forward+'s six float4 a light
/// (fillGlobalLightListBuffer's layout: position + type, diffuse + the light mask's
/// bits, specular, attenuation, spot direction + profile, spot parameters) behind one
/// header float4 (the count, the per-hit cap). ORDERED BY CONTRIBUTION — the peak
/// channel of the light's power, brightest first, the id as the tie — and the hit
/// shades at most Forward+'s lights-per-cell of the ones whose range contains it:
/// the bound the forward list keeps on screen. A light is visible by Ogre's own
/// test (the visibility layer and the scene's light mask). Area lights and
/// directional ones are the pass buffer's (directional always; area lights as
/// the camera culled them — stated). Uploaded only when the bytes change.
void HlmsAtom::buildWorldLights(Ogre::SceneManager *sm, WorldLights &out) {
    const std::vector<const Ogre::Light *> &casting = out.casting;
    const Ogre::uint32 cap = out.cap;
    struct Entry {
        const Ogre::Light *light = nullptr;
        float power = 0.0f;
    };
    std::vector<Entry> lights;
    const Ogre::uint32 mask = sm->getLightMask();
    Ogre::SceneManager::MovableObjectIterator it = sm->getMovableObjectIterator(Ogre::LightFactory::FACTORY_TYPE_NAME);
    while (it.hasMoreElements()) {
        const Ogre::Light *l = static_cast<const Ogre::Light *>(it.getNext());
        if (l->getType() != Ogre::Light::LT_POINT && l->getType() != Ogre::Light::LT_SPOTLIGHT) continue;
        if (!l->isAttached() || !l->getVisible() || !(l->getVisibilityFlags() & mask)) continue;
        if (std::find(casting.begin(), casting.end(), l) != casting.end()) continue;
        const Ogre::ColourValue d = l->getDiffuseColour() * l->getPowerScale();
        const Ogre::ColourValue sp = l->getSpecularColour() * l->getPowerScale();
        const float power = std::max(std::max(d.r, d.g), d.b);
        if (!(power > 0.0f) && !(std::max(std::max(sp.r, sp.g), sp.b) > 0.0f)) continue;
        if (!(l->getAttenuationRange() > 0.0f)) continue;
        lights.push_back({ l, power });
    }
    std::sort(lights.begin(), lights.end(), [](const Entry &a, const Entry &b) {
        if (a.power != b.power) return a.power > b.power;
        return a.light->getId() < b.light->getId();
    });
    const float invHeightProfiles = Ogre::Root::getSingleton().getLightProfilesInvHeight();
    std::vector<float> &data = out.data;
    data.clear();
    data.reserve(4u + lights.size() * 24u);
    const Ogre::uint32 count = Ogre::uint32(lights.size());
    auto bits = [](Ogre::uint32 u) { float f; std::memcpy(&f, &u, sizeof f); return f; };
    data.push_back(bits(count));
    data.push_back(bits(cap));
    data.push_back(0.0f);
    data.push_back(0.0f);
    for (const Entry &e : lights) {
        const Ogre::Light *l = e.light;
        const Ogre::Vector3 p = l->getParentNode()->_getDerivedPosition();
        data.insert(data.end(), { float(p.x), float(p.y), float(p.z), float(l->getType()) });
        const Ogre::ColourValue d = l->getDiffuseColour() * l->getPowerScale();
        data.insert(data.end(), { d.r, d.g, d.b, bits(l->getLightMask()) });
        const Ogre::ColourValue sp = l->getSpecularColour() * l->getPowerScale();
        data.insert(data.end(), { sp.r, sp.g, sp.b, 0.0f });
        const float range = float(l->getAttenuationRange());
        data.insert(data.end(), { range, float(l->getAttenuationLinear()), float(l->getAttenuationQuadric()),
                                  1.0f / range });
        const Ogre::Vector3 dir = l->getDerivedDirection();
        data.insert(data.end(), { float(dir.x), float(dir.y), float(dir.z),
                                  (float(l->getLightProfileIdx()) + 0.5f) * invHeightProfiles });
        const float inner = float(l->getSpotlightInnerAngle().valueRadians());
        const float outer = float(l->getSpotlightOuterAngle().valueRadians());
        data.insert(data.end(), { 1.0f / (std::cos(inner * 0.5f) - std::cos(outer * 0.5f)), std::cos(outer * 0.5f),
                                  float(l->getSpotlightFalloff()), 0.0f });
    }
    out.count = count;
}

void HlmsAtom::uploadWorldLights(Ogre::SceneManager *sm, const Ogre::CompositorShadowNode *shadowNode) {
    if (!mVaoManager || !sm) return;
    std::vector<const Ogre::Light *> casting;
    if (shadowNode)
        for (const Ogre::LightClosest &lc : shadowNode->getShadowCastingLights())
            if (lc.light) casting.push_back(lc.light);
    Ogre::uint32 cap = 96u;
    if (auto *fc = dynamic_cast<Ogre::ForwardClustered *>(sm->getForwardPlus())) cap = fc->getLightsPerCell();
    // THE DIRT (the brief's design: rebuilt when a light changes). A scene with no
    // binding record (none registered) is rebuilt every pass.
    unsigned long long serial = 0ull, removed = 0ull;
    const bool dirtKnown = sceneLightDirt(sm, serial, removed);
    WorldLights &wl = mWorldLightScenes[sm];
    const bool rebuild = !dirtKnown || wl.serial != serial || wl.removed != removed ||
                         wl.casting != casting || wl.cap != cap;
    if (rebuild) {
        wl.casting.swap(casting);
        wl.cap = cap;
        buildWorldLights(sm, wl);
        wl.serial = dirtKnown ? serial : ~0ull;
        wl.removed = dirtKnown ? removed : ~0ull;
    }
    const std::vector<float> &data = wl.data;
    const unsigned count = wl.count;
    mWorldLightCount = count;
    mWorldLightCap = wl.cap;
    if (mWorldLightBuf && data.size() == mWorldLightMirror.size() &&
        std::memcmp(data.data(), mWorldLightMirror.data(), data.size() * sizeof(float)) == 0)
        return;
    // A read-only buffer's element is one byte (see uploadBucketTable).
    const size_t bytes = data.size() * sizeof(float);
    if (mWorldLightBuf && mWorldLightBuf->getNumElements() < bytes) {
        mVaoManager->destroyReadOnlyBuffer(mWorldLightBuf);
        mWorldLightBuf = nullptr;
    }
    if (!mWorldLightBuf) {
        // Room for 32 lights before the first regrowth.
        const size_t room = std::max(bytes, (4u + 32u * 24u) * sizeof(float));
        mWorldLightBuf = mVaoManager->createReadOnlyBuffer(Ogre::PFG_RGBA32_FLOAT, room, Ogre::BT_DEFAULT,
                                                           nullptr, false);
    }
    mWorldLightBuf->upload(data.data(), 0u, bytes);
    mWorldLightMirror = data;
}

// ---------------------------------------------------------------------------
// THE PASS: the first read of a frame asks PBS what it holds (tellEveryHlms, once per frame).
// ---------------------------------------------------------------------------
void HlmsAtom::analyzeBarriers(Ogre::BarrierSolver &barrierSolver,
                               Ogre::ResourceTransitionArray &resourceTransitions,
                               Ogre::Camera *renderingCamera, const bool bCasterPass) {
    // NOTHING TO DECODE, NOTHING TO DO: every registered Hlms is asked this for
    // every scene pass of the frame, and this host draws only through its twins,
    // only in a pass whose recorder has set a decode source (the parity suite's
    // workspace, the chain's hit decode pass — PHOTON-HIT-SHADE-1's gate: the
    // product holds twins in every ray-traced scene, and PBS's pass prepare is
    // the expensive half of a pass).
    // ...AND NEVER IN A SHADOW PASS: a decode is never a caster, and the shadow
    // node's passes run INSIDE the armed pass (the prepass that executes the node).
    if (mTwins.empty() || !mSource.ids || bCasterPass) return;
    // THE FIRST THING A PASS ASKS OF US READS THE VOXEL AND FIELD TEXTURES — so the
    // relay and THIS PASS'S SCENE'S arms (bindSceneGi) come BEFORE, never after: an
    // arm from the previous pass's scene is the wrong one, and may be deleted.
    tellEveryHlms(mHlmsManager);
    bindSceneGi(this, renderingCamera ? renderingCamera->getSceneManager() : nullptr);
    Ogre::HlmsPbs::analyzeBarriers(barrierSolver, resourceTransitions, renderingCamera, bCasterPass);
    if (bCasterPass) return;
    // WHAT THE DECODE READS, declared to Ogre's solver like every other pass input:
    // the id image as a texture, the GPU scene's tables as read-only buffers in the
    // pixel stage (their writes are the frame's staging copies — without this the
    // read is an unordered hazard against them).
    if (mSource.ids)
        barrierSolver.resolveTransition(resourceTransitions, mSource.ids, Ogre::ResourceLayout::Texture,
                                        Ogre::ResourceAccess::Read, 1u << Ogre::PixelShader);
    for (Ogre::UavBufferPacked *b : { mSource.instances, mSource.levels, mSource.geomRows })
        if (b)
            barrierSolver.resolveTransition(resourceTransitions, b, Ogre::ResourceAccess::Read,
                                            1u << Ogre::PixelShader);
    // HIT MODE: the list's buffer (the vertex stage's count, the pixel stage's aux).
    if (mSource.hitMode && mSource.hitBuf)
        barrierSolver.resolveTransition(resourceTransitions, mSource.hitBuf, Ogre::ResourceAccess::Read,
                                        (1u << Ogre::VertexShader) | (1u << Ogre::PixelShader));
}

Ogre::HlmsCache HlmsAtom::preparePassHash(const Ogre::CompositorShadowNode *shadowNode,
                                          bool casterPass, bool dualParaboloid,
                                          Ogre::SceneManager *sceneManager) {
    // RenderQueue::renderPassPrepare asks EVERY registered Hlms for its pass hash on
    // EVERY scene pass (OgreRenderQueue.cpp), whether or not anything of its draws.
    // PBS's preparePassHash is the expensive half of a pass's CPU prepare (the pass
    // buffer, the lights, the shadow maps), and this host draws only through its
    // decode twins — so while there are none it costs the frame nothing: the cache
    // it returns is never looked up, because no renderable of this type is queued.
    if (mTwins.empty() || !mSource.ids || casterPass) {
        mPassSkipped = true;
        return Ogre::HlmsCache();
    }
    mPassSkipped = false;
    tellEveryHlms(mHlmsManager);
    bindSceneGi(this, sceneManager);
    // Every upload of the pass happens here, before its first draw begins the
    // render pass: the stand-ins every draw binds, the bucket table.
    ensureStandIns();
    uploadBucketTable();
    if (mSource.hitMode && !casterPass) uploadWorldLights(sceneManager, shadowNode);
    Ogre::HlmsCache ret =
        Ogre::HlmsPbs::preparePassHash(shadowNode, casterPass, dualParaboloid, sceneManager);
    if (casterPass) return ret;
    if (!mSource.hitMode) {
        if (!mSource.classified) return ret;
        // A SCREEN DECODE PASS (ATOM-DECODE-CLASS-1): one pass property on top of
        // PBS's, the pass cache re-keyed on it exactly as hit mode's below —
        //   atom_classified   the bucket draws' vertex stage puts the triangle at
        //                     the draw's class depth, their pixel stage declares
        //                     early fragment tests, and their PSO tests that depth
        //                     EQUAL without writing it (applyStrongMacroblockRules).
        Ogre::HlmsPropertyVec props = ret.setProperties;
        setProperty(props, Ogre::IdString("atom_classified"), 1);
        // THE DISCRIMINATOR (a TEST DOOR, ATOM-BLACK-FRAMES-1): a colour code per failed
        // validity term instead of the discard (800.Atom_piece_ps.any's AtomDeclDecode);
        // 2 paints the code chart. Read per pass: a test flips it between frames.
        if (const char *d = std::getenv("JAHSHAKA_ATOM_DISCRIMINATE")) {
            const int v = std::atoi(d);
            if (v >= 1) setProperty(props, Ogre::IdString("atom_discriminate"), 1);
            if (v >= 2) setProperty(props, Ogre::IdString("atom_discriminate_chart"), 1);
        }
        PassCache passCache;
        passCache.passPso = ret.pso.pass;
        passCache.properties = props;
        size_t passIdx = 0u;
        findOrAddPassCache(passCache, true, passIdx);
        ret.hash = static_cast<Ogre::uint32>(passIdx) << Ogre::HlmsBits::PassShift;
        ret.setProperties = props;
        return ret;
    }
    // HIT MODE (PHOTON-HIT-SHADE-1): three pass properties on top of PBS's, and
    // the pass cache re-keyed on them (PBS built it from its own set):
    //   atom_hit_mode                     the decode's four hit branches;
    //   hlms_forwardplus_custom_frag_coord the fork's Forward+ cell hook — the
    //                                     decode hands the hit's own pixel;
    //   hlms_fog = 0                      the camera's distance fog is not a
    //                                     hit's (its radiance leaves the hit
    //                                     towards the ray's origin); the pass
    //                                     buffer's later members are padded for
    //                                     it by the fog piece's own layout rule.
    //
    // THE BASE IS THE RETURNED SET, never mT[kNoTid] (measured in the app: PBS's
    // pass prepare leaves 14 of its 69 properties in mT by the time it returns —
    // a pass cache built from mT lost the lights and the pass buffer).
    Ogre::HlmsPropertyVec props = ret.setProperties;
    setProperty(props, Ogre::IdString("atom_hit_mode"), 1);
    setProperty(props, Ogre::IdString("hlms_forwardplus_custom_frag_coord"), 1);
    setProperty(props, Ogre::HlmsBaseProp::Fog, 0);
    // MEASUREMENT DOORS (D3-HIT-SHADE-2's paired arms; read per pass, so one process
    // flips them between frames — never a product setting):
    //   JAHSHAKA_HIT_WORLD_LIGHTS=off   the world light list is not read (the
    //                                   picture before this lane: a hit with no
    //                                   cell gets no point or spot light);
    //   JAHSHAKA_HIT_WORLD_LIGHTS=all   EVERY hit takes the world list and no
    //                                   Forward+ cell (the "one list" arm);
    //   JAHSHAKA_HIT_VCT_SPECULAR=0     the VCT specular cone compiled out of the
    //                                   hit decode (vct_disable_specular).
    if (const char *w = std::getenv("JAHSHAKA_HIT_WORLD_LIGHTS")) {
        if (std::strcmp(w, "off") == 0) setProperty(props, Ogre::IdString("atom_hit_world_off"), 1);
        else if (std::strcmp(w, "all") == 0) setProperty(props, Ogre::IdString("atom_hit_world_all"), 1);
    }
    if (const char *v = std::getenv("JAHSHAKA_HIT_VCT_SPECULAR"); v && std::strcmp(v, "0") == 0)
        setProperty(props, Ogre::IdString("vct_disable_specular"), 1);
    PassCache passCache;
    passCache.passPso = ret.pso.pass;
    passCache.properties = props;
    size_t passIdx = 0u;
    findOrAddPassCache(passCache, true, passIdx);
    ret.hash = static_cast<Ogre::uint32>(passIdx) << Ogre::HlmsBits::PassShift;
    ret.setProperties = props;
    return ret;
}

void HlmsAtom::postCommandBufferExecution(Ogre::CommandBuffer *commandBuffer) {
    if (mPassSkipped) {
        Ogre::HlmsBufferManager::postCommandBufferExecution(commandBuffer);
        return;
    }
    Ogre::HlmsPbs::postCommandBufferExecution(commandBuffer);
}

Ogre::Hlms::PropertiesMergeStatus HlmsAtom::notifyPropertiesMergedPreGenerationStep(
    size_t tid, Ogre::PiecesMap *inOutPieces) {
    const PropertiesMergeStatus status =
        Ogre::HlmsPbs::notifyPropertiesMergedPreGenerationStep(tid, inOutPieces);
    if (status == PropertiesMergeStatusError) return status;
    // THE RESERVED REGISTERS, named exactly where Terra names its own
    // (OgreHlmsTerra.cpp:429-438), and derived only from properties already in the
    // merged set, so the shader cache still keys on the same thing.
    setProperty(tid, "atomInstanceBuf", kInstanceBufSlot);
    setProperty(tid, "atomLevelBuf", kLevelBufSlot);
    setProperty(tid, "atomGeomRowBuf", kGeomRowBufSlot);
    setProperty(tid, "atomBucketBuf", kBucketBufSlot);
    setProperty(tid, "atomHitBuf", kHitBufSlot);
    setProperty(tid, "atomMeshBuf", kMeshBufSlot);
    setProperty(tid, "atomClusterBuf", kClusterBufSlot);
    setProperty(tid, "atomWorldLightBuf", kWorldLightBufSlot);
    setProperty(tid, "atomSlotsPerPool", Ogre::int32(mSlotsPerPool));
    Ogre::int32 texSlotsStart = kReservedBufSlots;
    if (getProperty(tid, Ogre::HlmsBaseProp::ForwardPlus))
        texSlotsStart = getProperty(tid, "f3dGrid") + 1;
    if (!getProperty(tid, Ogre::HlmsBaseProp::ShadowCaster))
        setTextureReg(tid, Ogre::PixelShader, "atomIdTex", texSlotsStart);
    return status;
}

void HlmsAtom::calculateHashForPreCreate(Ogre::Renderable *renderable, Ogre::PiecesMap *inOutPieces) {
    Ogre::HlmsPbs::calculateHashForPreCreate(renderable, inOutPieces);
    if (mClassifyDb && renderable && renderable->getDatablock() == mClassifyDb)
        setProperty(kNoTid, Ogre::IdString("atom_classify"), 1);
}

void HlmsAtom::applyStrongMacroblockRules(Ogre::HlmsMacroblock &macroblock, const size_t tid) const {
    Ogre::HlmsPbs::applyStrongMacroblockRules(macroblock, tid);
    // A BUCKET DRAW OF A CLASSIFIED PASS: its class's depth, EQUAL, never written —
    // the classifier keeps its own macroblock (always, written).
    if (getProperty(tid, Ogre::IdString("atom_classified")) &&
        !getProperty(tid, Ogre::IdString("atom_classify"))) {
        macroblock.mDepthCheck = true;
        macroblock.mDepthWrite = false;
        macroblock.mDepthFunc = Ogre::CMPF_EQUAL;
    }
}

void HlmsAtom::setupRootLayout(Ogre::RootLayout &rootLayout, size_t tid) {
    Ogre::HlmsPbs::setupRootLayout(rootLayout, tid);
    // PBS's range covers [0, worldMatBuf + 1) and, with Forward Clustered on,
    // [0, f3dLightList + 1) — which already contains our reserved slots, because the
    // reservation pushed f3dLightList above them. Without Forward Clustered it would
    // not, so the range is widened explicitly: a slot inside the root layout but
    // never bound is an undefined descriptor.
    Ogre::DescBindingRange *ranges = rootLayout.mDescBindingRanges[0];
    if (ranges[Ogre::DescBindingTypes::ReadOnlyBuffer].end < kReservedBufSlots) {
        ranges[Ogre::DescBindingTypes::ReadOnlyBuffer].start = 0u;
        ranges[Ogre::DescBindingTypes::ReadOnlyBuffer].end = kReservedBufSlots;
        if (ranges[Ogre::DescBindingTypes::Texture].start < kReservedBufSlots)
            ranges[Ogre::DescBindingTypes::Texture].start = kReservedBufSlots;
        if (ranges[Ogre::DescBindingTypes::Sampler].start < ranges[Ogre::DescBindingTypes::Texture].start)
            ranges[Ogre::DescBindingTypes::Sampler].start = ranges[Ogre::DescBindingTypes::Texture].start;
    }
}

Ogre::uint32 HlmsAtom::fillBuffersForV2(const Ogre::HlmsCache *cache,
                                        const Ogre::QueuedRenderable &queuedRenderable,
                                        bool casterPass, Ogre::uint32 lastCacheHash,
                                        Ogre::CommandBuffer *commandBuffer) {
    const Ogre::uint32 ret = Ogre::HlmsPbs::fillBuffersForV2(cache, queuedRenderable, casterPass,
                                                             lastCacheHash, commandBuffer);
    if (casterPass) return ret;

    // THE PASS RESOURCES, on the Hlms-type switch exactly like PBS's own
    // (OgreHlmsPbs.cpp fillBuffersFor), in the slots reserved for them. The GPU
    // scene's tables are bound as READ-ONLY VIEWS of their UAV buffers, fetched here
    // and never cached: a table is re-created when it grows.
    //
    // EVERY SLOT IS BOUND, ALWAYS — a real source or the host's empty stand-in. A slot
    // this draw did not bind still holds whatever the previous pass left there
    // (measured on the RTX: an RGBA8 texture read as the id image, garbage ids, a GPU
    // hang), and a wrong device address in a shader is never a validation error.
    if (OGRE_EXTRACT_HLMS_TYPE_FROM_CACHE_HASH(lastCacheHash) != mType) {
        const bool complete = mSource.ids && mSource.instances && mSource.levels && mSource.geomRows;
        if (!complete) {
            ensureStandIns();
            if (mSourcelessDraws++ == 0u) {
                const Ogre::Camera *cam =
                    queuedRenderable.movableObject && queuedRenderable.movableObject->_getManager()
                        ? queuedRenderable.movableObject->_getManager()->getCamerasInProgress().renderingCamera
                        : nullptr;
                Ogre::LogManager::getSingleton().logMessage(
                    "HlmsAtom: a decode draw was recorded with no decode source (camera '" +
                        (cam ? cam->getName() : Ogre::String("?")) +
                        "'); it binds the empty stand-ins and shades nothing. Logged once.",
                    Ogre::LML_CRITICAL);
            }
        }
        auto roView = [this](Ogre::UavBufferPacked *b) -> Ogre::ReadOnlyBufferPacked * {
            return b ? b->getAsReadOnlyBufferView() : mEmptyBuf;
        };
        const bool whole = complete;
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kInstanceBufSlot, whole ? roView(mSource.instances) : mEmptyBuf, 0, 0);
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kLevelBufSlot, whole ? roView(mSource.levels) : mEmptyBuf, 0, 0);
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kGeomRowBufSlot, whole ? roView(mSource.geomRows) : mEmptyBuf, 0, 0);
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kBucketBufSlot, (whole && mBucketBuf) ? mBucketBuf : mEmptyBuf, 0, 0);
        // HIT MODE's list buffer (both stages read it) — the stand-in otherwise
        // (made in preparePassHash: an upload here would land INSIDE the pass'
        // render pass, which the pin cannot resume with a clear).
        const bool hit = whole && mSource.hitMode && mSource.hitBuf;
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kHitBufSlot, hit ? roView(mSource.hitBuf) : mEmptyBuf, 0, 0);
        // THE CUT'S TABLES (the screen decode's cluster resolve).
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kMeshBufSlot, (whole && mSource.meshes) ? roView(mSource.meshes) : mEmptyBuf, 0, 0);
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kClusterBufSlot,
            (whole && mSource.clusters) ? roView(mSource.clusters) : mEmptyBuf, 0, 0);
        // HIT MODE's world light list (uploadWorldLights) — the stand-in otherwise
        // (its first word, the count, is 0).
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, kWorldLightBufSlot, (hit && mWorldLightBuf) ? mWorldLightBuf : mEmptyBuf, 0, 0);
        Ogre::uint16 texSlot = kReservedBufSlots;
        if (mGridBuffer) texSlot = Ogre::uint16(texSlot + 2u);
        *commandBuffer->addCommand<Ogre::CbTexture>() =
            Ogre::CbTexture(texSlot, whole ? mSource.ids : mEmptyIds, mPointSampler);
    }

    // THE MATERIAL BUFFER IS PBS's POOL, NOT OURS. PBS just bound the twin's own pool
    // at const-buffer slot 1 (`materialArray`); the id buffer's material word names
    // the PBS datablock's slot in the PBS pool, so that pool replaces it — per draw,
    // because each twin may serve a different pool. The twin's own constants are never
    // read. mLastBoundPool is forgotten so the next draw's PBS half rebinds whatever
    // it needs rather than trusting a slot we overwrote.
    const Ogre::HlmsDatablock *db = queuedRenderable.renderable->getDatablock();
    // THE DRAW'S CLASS, in the .w of the per-draw word PBS just wrote (its four
    // uints end at mCurrentMappedConstBuffer; .w is the planar-reflection index, which
    // no twin's permutation reads — a twin serves a bucket, never a planar renderable):
    // the vertex stage's depth in a classified pass, the pixel stage's bucket test.
    if (auto it = mTwins.find(db); it != mTwins.end())
        *(mCurrentMappedConstBuffer - 1) = it->second.classIdx;
    if (auto it = mTwins.find(db); it != mTwins.end() && it->second.pbs &&
                                   it->second.pbs->getAssignedPool()) {
        const Ogre::ConstBufferPool::BufferPool *pool = it->second.pbs->getAssignedPool();
        *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
            Ogre::PixelShader, 1, pool->materialBuffer, 0,
            Ogre::uint32(pool->materialBuffer->getTotalSizeBytes()));
        mLastBoundPool = nullptr;
    }
    return ret;
}

// ---------------------------------------------------------------------------
// THE FULL-SCREEN TRIANGLE
// ---------------------------------------------------------------------------
AtomDecodeRenderable::AtomDecodeRenderable(Ogre::IdType id,
                                           Ogre::ObjectMemoryManager *objectMemoryManager,
                                           Ogre::SceneManager *manager, Ogre::uint8 renderQueueId)
    : Ogre::MovableObject(id, objectMemoryManager, manager, renderQueueId), Ogre::Renderable() {
    // An infinite AABB: a full-screen triangle must never be frustum-culled.
    const Ogre::Aabb aabb(Ogre::Aabb::BOX_INFINITE);
    mObjectData.mLocalAabb->setFromAabb(aabb, mObjectData.mIndex);
    mObjectData.mWorldAabb->setFromAabb(aabb, mObjectData.mIndex);
    mObjectData.mLocalRadius[mObjectData.mIndex] = std::numeric_limits<Ogre::Real>::max();
    mObjectData.mWorldRadius[mObjectData.mIndex] = std::numeric_limits<Ogre::Real>::max();
    mVao = createFullScreenVao(manager->getDestinationRenderSystem()->getVaoManager());
    mVaoPerLod[Ogre::VpNormal].push_back(mVao);
    mVaoPerLod[Ogre::VpShadow].push_back(mVao);
    mRenderables.push_back(this);
    setCastShadows(false);
}

AtomDecodeRenderable::~AtomDecodeRenderable() {
    // THE OWNER DESTROYS THIS WHILE Root — and so the VaoManager — IS ALIVE (the
    // startup/teardown trap: a geometry object outliving Root throws in VaoManager).
    mVaoPerLod[Ogre::VpNormal].clear();
    mVaoPerLod[Ogre::VpShadow].clear();
    if (mVao && mManager)
        destroyFullScreenVao(mManager->getDestinationRenderSystem()->getVaoManager(), mVao);
    mVao = nullptr;
}

const Ogre::String &AtomDecodeRenderable::getMovableType() const { return Ogre::BLANKSTRING; }
const Ogre::LightList &AtomDecodeRenderable::getLights() const { return this->queryLights(); }
void AtomDecodeRenderable::getRenderOperation(Ogre::v1::RenderOperation &, bool) {
    OGRE_EXCEPT(Ogre::Exception::ERR_NOT_IMPLEMENTED, "v2 only", "AtomDecodeRenderable");
}
void AtomDecodeRenderable::getWorldTransforms(Ogre::Matrix4 *) const {
    OGRE_EXCEPT(Ogre::Exception::ERR_NOT_IMPLEMENTED, "v2 only", "AtomDecodeRenderable");
}
bool AtomDecodeRenderable::getCastsShadows() const { return false; }

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
